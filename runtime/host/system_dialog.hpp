#pragma once

// The PSP system utility dialog a game has open (message, savedata or keyboard), described for a host
// that draws it and lets the player answer through Kernel::dialog_*. Only interactive kernels
// (Kernel::set_interactive_dialogs) open one; otherwise dialogs are answered at once.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pspweb {

struct SystemDialog {
    enum class Kind : std::uint8_t { Message, SaveList, Keyboard };
    enum class Heading : std::uint8_t { None, Load, Save, Delete };
    // The system's own texts; the host translates them. Custom is the game's text.
    enum class Text : std::uint8_t {
        Custom, Error, NoData, ConfirmLoad, ConfirmSave, ConfirmDelete, ConfirmOverwrite,
        LoadDone, SaveDone, DeleteDone, LoadFailed, SaveFailed, DeleteFailed
    };
    enum class Buttons : std::uint8_t { Ok, YesNo };

    struct Item {
        std::string name;  // the save folder's suffix after the game id
        bool exists{};     // false: a new save ("New data")
        std::string title, savedata_title, detail;
        std::int64_t time{};          // newest file, seconds since 1970 (0 for new data)
        std::filesystem::path icon;   // ICON0.PNG, empty when there is none
    };

    Kind kind{Kind::Message};
    Heading heading{Heading::None};
    Text text_id{Text::Custom};
    std::string text;       // Custom: the game's message (UTF-8)
    std::uint32_t error{};  // Error: the code, shown as (80XXXXXX)
    Buttons buttons{Buttons::Ok};
    std::vector<Item> items;  // SaveList
    int focus{};              // SaveList: the item; YesNo: 0 Yes, 1 No
    bool cross_confirms{true};  // the game's button layout; false: Circle confirms, Cross goes back
    std::uint64_t serial{};     // changes with every dialog and every step of one
    // Keyboard: the text typed so far (UTF-8, the game's description is `text`), at most
    // max_length characters, and focus is the key in kOskKeys.
    std::string input;
    int max_length{};
};

// The on-screen keyboard's keys, kOskColumns to a row; the last three are actions.
inline constexpr int kOskColumns = 10;
inline constexpr const char *kOskKeys[] = {
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J",
    "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T",
    "U", "V", "W", "X", "Y", "Z", ".", ",", "-", "'",
    "a", "b", "c", "d", "e", "f", "g", "h", "i", "j",
    "k", "l", "m", "n", "o", "p", "q", "r", "s", "t",
    "u", "v", "w", "x", "y", "z", "!", "?", "_", "/",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "Space", "Delete", "Done",
};
inline constexpr int kOskKeyCount = static_cast<int>(sizeof kOskKeys / sizeof kOskKeys[0]);
inline constexpr int kOskSpace = kOskKeyCount - 3, kOskDelete = kOskKeyCount - 2, kOskDone = kOskKeyCount - 1;

} // namespace pspweb
