#pragma once
#include <string>

// The player's own profile picture from a file: avatar.png / avatar.jpg / avatar.jpeg next to the mod's DLL (or in
// the settings folder, %LOCALAPPDATA%\MultiDiva2). It is sent to the server in `hello` and served to the others like
// any custom avatar, and it is shown instead of the Steam picture.
namespace md::ui {

// Base64 of a PNG/JPEG the server accepts (at most 32 KB and 256 px a side), made from the file: used as it is when
// it already fits, else cropped to a square, shrunk and re-encoded. "" when there is no file or it can't be used.
// Read once and kept; call from the game thread.
const std::string& OwnAvatarBase64();

} // namespace md::ui
