// Pressing A for you at the start of a chapter.
//
// Every chapter opens with an in-engine cinematic that has to be dismissed by
// hand, which on a replay is pure friction - and this port is mostly played by
// people replaying.
//
// It works by adding a synthetic controller. The input system merges every
// device assigned to a player, so a virtual pad that reports A and START held
// is ORed into whatever the real controller says. Nothing in the guest is
// patched and nothing about the real pad changes.
//
// Two things keep it from becoming a nuisance, and both matter more than the
// feature itself:
//
//   * it is ARMED by a chapter load, not left running. The port already sees
//     the guest's file opens, so a story file opening is the signal, and the
//     arming expires on its own.
//   * REAL INPUT DISARMS IT IMMEDIATELY. The moment the player touches the pad
//     the synthetic presses stop, so a cinematic they wanted to watch is one
//     stick nudge away from being left alone - and it can never be pressing A
//     during play, which in this game means attacking.

#pragma once

#include <memory>

#include <rex/input/input_driver.h>

namespace ng2 {

// Arms the skip for the next few seconds. Called when the guest opens a file
// that indicates a chapter is starting.
void ArmAutoSkip();

// Any genuine input from the player. Disarms immediately.
void NoteRealInput();

// Whether the feature is switched on at all.
void SetAutoSkipEnabled(bool enabled);
bool AutoSkipActive();

// The synthetic pad. Owned by the input system once added.
std::unique_ptr<rex::input::InputDriver> MakeAutoSkipDriver();

}  // namespace ng2
