// Which chapter the game is currently loading or playing.
//
// Set by the file-open hook in diag_hooks.cpp, which sees the game open
// s_chap_NN.ng2 for the chapter it is loading. It exists so a workaround can be
// scoped to the chapter that needs it instead of being a global switch: the
// community Chapter 12 patch is documented as causing problems elsewhere, and
// "elsewhere" is exactly what this can now exclude.
//
// 0 means nothing has loaded a chapter yet.

#pragma once

namespace ng2 {

int CurrentChapter();

}  // namespace ng2
