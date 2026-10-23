// SPDX-License-Identifier: GPL-2.0-or-later

#include "jumptables.hpp"

#include "psprecomp/decoder.hpp"

#include <map>

namespace anypsp::analysis {
namespace {

using psprecomp::DecodedInstruction;
using psprecomp::OpcodeKind;

constexpr std::size_t kWindow = 32u;
constexpr std::uint32_t kRa = 31u;
constexpr std::uint32_t kGp = 28u;

// value = (variable << shift) + offset, a constant, or a word loaded from table + (variable << 2).
struct Value {
    enum class Kind : std::uint8_t { Constant, Linear, TableLoad } kind{Kind::Linear};
    std::uint32_t constant{};
    int variable{};
    std::uint32_t shift{};
    std::uint32_t offset{};
};

class Evaluator {
public:
    explicit Evaluator(std::uint32_t gp) {
        for (std::uint32_t r = 0; r < 32u; ++r) regs_[r] = fresh();
        regs_[0] = constant(0u);
        if (gp != 0u) regs_[kGp] = constant(gp);
    }

    void step(const DecodedInstruction &d, std::uint32_t pc, const psprecomp::GuestMemory &memory) {
        const Value &s = regs_[d.rs];
        const Value &t = regs_[d.rt];
        const auto simm = static_cast<std::uint32_t>(static_cast<std::int32_t>(d.immediate));
        const auto uimm = static_cast<std::uint32_t>(static_cast<std::uint16_t>(d.immediate));
        switch (d.kind) {
        case OpcodeKind::Lui: set(d.rt, constant(uimm << 16u)); return;
        case OpcodeKind::Addiu: set(d.rt, add(s, simm)); return;
        case OpcodeKind::Ori:
            set(d.rt, s.kind == Value::Kind::Constant ? constant(s.constant | uimm) : fresh());
            return;
        case OpcodeKind::Addu:
        case OpcodeKind::Or:
            if (d.kind == OpcodeKind::Or && !is_zero(s) && !is_zero(t)) {
                set(d.rd, both_constant(s, t) ? constant(s.constant | t.constant) : fresh());
                return;
            }
            set(d.rd, sum(s, t));
            return;
        case OpcodeKind::Sll:
            if (t.kind == Value::Kind::Constant) set(d.rd, constant(t.constant << d.sa));
            else if (t.kind == Value::Kind::Linear) set(d.rd, shifted(t, d.sa));
            else set(d.rd, fresh());
            return;
        case OpcodeKind::Sltiu:
            if (s.kind == Value::Kind::Linear && s.shift == 0u) bounds_[{s.variable, s.offset}] = simm;
            set(d.rt, fresh());
            return;
        case OpcodeKind::Sltu:
            if (s.kind == Value::Kind::Linear && s.shift == 0u && t.kind == Value::Kind::Constant)
                bounds_[{s.variable, s.offset}] = t.constant;
            set(d.rd, fresh());
            return;
        case OpcodeKind::Lw: {
            const std::uint32_t displacement = simm;
            if (s.kind == Value::Kind::Linear && s.shift == 2u) {
                Value loaded;
                loaded.kind = Value::Kind::TableLoad;
                loaded.variable = s.variable;
                loaded.constant = s.offset + displacement;
                set(d.rt, loaded);
            } else if (s.kind == Value::Kind::Constant && memory.contains(s.constant + displacement, 4u) &&
                       ((s.constant + displacement) & 3u) == 0u) {
                set(d.rt, constant(memory.load32(s.constant + displacement)));
            } else {
                set(d.rt, fresh());
            }
            return;
        }
        case OpcodeKind::Jal: case OpcodeKind::Bltzal: case OpcodeKind::Bgezal:
        case OpcodeKind::Bltzall: case OpcodeKind::Bgezall:
            set(kRa, constant(pc + 8u));
            return;
        case OpcodeKind::Jalr: set(d.rd, constant(pc + 8u)); return;
        case OpcodeKind::Syscall: case OpcodeKind::Vfpu: case OpcodeKind::Unsupported:
            for (std::uint32_t r = 1; r < 32u; ++r) if (r != kGp) regs_[r] = fresh();
            return;
        default: break;
        }
        if (const auto written = destination(d)) set(*written, fresh());
    }

    [[nodiscard]] const Value &reg(std::uint32_t r) const { return regs_[r]; }
    [[nodiscard]] std::optional<std::uint32_t> bound(int variable, std::uint32_t offset) const {
        const auto it = bounds_.find({variable, offset});
        if (it == bounds_.end()) return std::nullopt;
        return it->second;
    }

private:
    static bool is_zero(const Value &v) { return v.kind == Value::Kind::Constant && v.constant == 0u; }
    static bool both_constant(const Value &a, const Value &b) {
        return a.kind == Value::Kind::Constant && b.kind == Value::Kind::Constant;
    }
    static Value constant(std::uint32_t c) {
        Value v;
        v.kind = Value::Kind::Constant;
        v.constant = c;
        return v;
    }
    Value fresh() {
        Value v;
        v.variable = next_variable_++;
        return v;
    }
    Value add(const Value &v, std::uint32_t c) {
        if (v.kind == Value::Kind::Constant) return constant(v.constant + c);
        if (v.kind == Value::Kind::Linear) {
            Value r = v;
            r.offset += c;
            return r;
        }
        return fresh();
    }
    Value sum(const Value &a, const Value &b) {
        if (a.kind == Value::Kind::Constant) return add(b, a.constant);
        if (b.kind == Value::Kind::Constant) return add(a, b.constant);
        return fresh();
    }
    Value shifted(const Value &v, std::uint32_t sa) {
        if (v.shift != 0u) return fresh();
        if (v.offset == 0u) {
            Value r = v;
            r.shift = sa;
            return r;
        }
        // (x + c) << sa: name x + c as a new variable and carry its bound over.
        Value r = fresh();
        r.shift = sa;
        if (const auto b = bound(v.variable, v.offset)) bounds_[{r.variable, 0u}] = *b;
        return r;
    }
    void set(std::uint32_t r, const Value &v) {
        if (r != 0u) regs_[r] = v;
    }
    static std::optional<std::uint32_t> destination(const DecodedInstruction &d) {
        switch (d.kind) {
        case OpcodeKind::Slti: case OpcodeKind::Andi: case OpcodeKind::Xori:
        case OpcodeKind::Lwl: case OpcodeKind::Lwr: case OpcodeKind::Lh: case OpcodeKind::Lhu:
        case OpcodeKind::Lb: case OpcodeKind::Lbu: case OpcodeKind::Mfc1: case OpcodeKind::Cfc1:
        case OpcodeKind::Mfv: case OpcodeKind::Ext: case OpcodeKind::Ins:
            return d.rt;
        case OpcodeKind::Add: case OpcodeKind::Sub: case OpcodeKind::Subu: case OpcodeKind::And:
        case OpcodeKind::Xor: case OpcodeKind::Nor: case OpcodeKind::Slt: case OpcodeKind::Max:
        case OpcodeKind::Min: case OpcodeKind::Movz: case OpcodeKind::Movn: case OpcodeKind::Srl:
        case OpcodeKind::Sra: case OpcodeKind::Rotr: case OpcodeKind::Sllv: case OpcodeKind::Srlv:
        case OpcodeKind::Srav: case OpcodeKind::Rotrv: case OpcodeKind::Clz: case OpcodeKind::Clo:
        case OpcodeKind::Seb: case OpcodeKind::Seh: case OpcodeKind::Bitrev: case OpcodeKind::Wsbh:
        case OpcodeKind::Wsbw: case OpcodeKind::Mfhi: case OpcodeKind::Mflo:
            return d.rd;
        default:
            return std::nullopt;
        }
    }

    std::array<Value, 32> regs_{};
    std::map<std::pair<int, std::uint32_t>, std::uint32_t> bounds_;
    int next_variable_{1};
};

bool is_likely(OpcodeKind kind) {
    switch (kind) {
    case OpcodeKind::Beql: case OpcodeKind::Bnel: case OpcodeKind::Blezl: case OpcodeKind::Bgtzl:
    case OpcodeKind::Bltzl: case OpcodeKind::Bgezl: case OpcodeKind::Bltzall: case OpcodeKind::Bgezall:
    case OpcodeKind::Bc1fl: case OpcodeKind::Bc1tl: case OpcodeKind::Bvfl: case OpcodeKind::Bvtl:
        return true;
    default:
        return false;
    }
}

// A path can be extended backwards over `pc` only when the instruction two words
// before it falls through: anything but a conditional branch ends the path.
bool falls_through_to(const DecodedInstruction &previous) {
    if (!previous.has_delay_slot()) return true;
    switch (previous.kind) {
    case OpcodeKind::J: case OpcodeKind::Jal: case OpcodeKind::Jr: case OpcodeKind::Jalr:
        return false;
    case OpcodeKind::Beq:
        return !(previous.rs == 0u && previous.rt == 0u);
    default:
        return true;
    }
}

// Runs the straight-line code ending at `last` (inclusive) through an evaluator.
Evaluator evaluate_path(const psprecomp::GuestMemory &memory, const std::vector<ExecutableRange> &ranges,
                        std::uint32_t last, std::uint32_t gp) {
    std::uint32_t start = last;
    for (std::size_t i = 0; i < kWindow; ++i) {
        const std::uint32_t candidate = start - 4u;
        if (!psprecomp::is_executable_address(ranges, candidate)) break;
        // `candidate` must not be the delay slot of a transfer that leaves the path.
        if (psprecomp::is_executable_address(ranges, candidate - 4u) &&
            !falls_through_to(psprecomp::decode_allegrex(memory.load32(candidate - 4u))))
            break;
        start = candidate;
    }
    Evaluator evaluator(gp);
    for (std::uint32_t pc = start; pc <= last; pc += 4u) {
        const auto d = psprecomp::decode_allegrex(memory.load32(pc));
        evaluator.step(d, pc, memory);
        // On the fall-through path a likely branch nullifies its delay slot.
        if (is_likely(d.kind) && pc + 4u <= last) pc += 4u;
    }
    return evaluator;
}

} // namespace

std::optional<JumpTable> resolve_jump_table(const psprecomp::GuestMemory &memory, const std::vector<ExecutableRange> &ranges,
                                            std::uint32_t jump_pc, std::uint32_t gp) {
    const auto jump = psprecomp::decode_allegrex(memory.load32(jump_pc));
    if (jump.kind != OpcodeKind::Jr || jump.rs == kRa || jump_pc < 4u) return std::nullopt;
    const Evaluator evaluator = evaluate_path(memory, ranges, jump_pc - 4u, gp);
    const Value &target = evaluator.reg(jump.rs);
    if (target.kind != Value::Kind::TableLoad) return std::nullopt;

    JumpTable table;
    table.jump_pc = jump_pc;
    table.table = target.constant;
    const auto bound = evaluator.bound(target.variable, 0u);
    table.bounded = bound.has_value() && *bound > 0u && *bound <= 0x10000u;
    const std::size_t limit = table.bounded ? *bound : kMaxUnboundedTableEntries;
    for (std::size_t i = 0; i < limit; ++i) {
        const std::uint32_t entry = table.table + static_cast<std::uint32_t>(i) * 4u;
        if (!memory.contains(entry, 4u)) return std::nullopt;
        const std::uint32_t code = memory.load32(entry);
        if (!psprecomp::is_executable_address(ranges, code)) {
            if (table.bounded) return std::nullopt; // a checked table must hold code addresses only
            break;
        }
        table.targets.push_back(code);
    }
    if (table.targets.empty()) return std::nullopt;
    return table;
}

std::array<std::optional<std::uint32_t>, 32> constants_at_call(const psprecomp::GuestMemory &memory,
                                                              const std::vector<ExecutableRange> &ranges,
                                                              std::uint32_t call_pc, std::uint32_t gp) {
    std::array<std::optional<std::uint32_t>, 32> result{};
    if (!psprecomp::is_executable_address(ranges, call_pc + 4u)) return result;
    const Evaluator evaluator = evaluate_path(memory, ranges, call_pc + 4u, gp);
    for (std::uint32_t r = 0; r < 32u; ++r)
        if (evaluator.reg(r).kind == Value::Kind::Constant) result[r] = evaluator.reg(r).constant;
    return result;
}

} // namespace anypsp::analysis
