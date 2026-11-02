#include "framekeyboard/key_sounds.hpp"

#include <iostream>
#include <stdexcept>

using framekeyboard::KeySoundActivity;
namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
void hover_and_departure() {
    KeySoundActivity activity;
    require(!activity.update(0, true, true), "showing the keyboard alone does not start audio");
    activity.pointer(0, true, 1);
    require(activity.update(1, true, true), "the first pointer prewarms audio before a key");
    require(activity.update(100, true, true), "a stationary pointer keeps audio warm");
    activity.pointer(1, true, 100);
    activity.pointer(0, false, 101);
    require(activity.update(110, true, true), "one remaining pointer keeps audio warm");
    activity.pointer(1, false, 110);
    require(activity.update(111.99, true, true), "the final departure has a two-second grace");
    activity.pointer(1, false, 111.99);
    require(!activity.update(112, true, true), "repeated leave events do not extend the deadline");
    require(!activity.update(120, true, true), "idle audio stays off without more pointer events");
    activity.pointer(7, true, 121);
    require(activity.update(121, true, true), "a later pointer starts a new warm session");
    activity.pointer(7, false, 122);
    activity.pointer(9, true, 123.99);
    require(activity.update(124, true, true), "reentering cancels the previous deadline");
    activity.pointer(9, false, 125);
    require(activity.update(126.99, true, true), "the new departure gets its own grace");
    require(!activity.update(127, true, true), "the renewed grace remains bounded");
}
void suppression_and_reopening() {
    KeySoundActivity activity;
    activity.pointer(2, true, 1);
    require(!activity.update(1, true, false), "muted hover does not start audio");
    require(activity.update(2, true, true), "unmuting with a pointer inside starts audio");
    require(!activity.update(2.1, true, false), "muting closes audio immediately while hovered");
    activity.pointer(2, false, 3);
    require(!activity.update(3.1, true, true), "unmuting after departure does not start audio");
    activity.pointer(3, true, 4);
    require(activity.update(4, true, true), "hover can restart after mute");
    activity.pointer(3, false, 5);
    require(!activity.update(5.1, false, true), "hiding cuts the departure grace immediately");
    require(!activity.update(5.2, true, true), "reopening does not inherit a warm stream");
    activity.pointer(4, true, 6);
    require(activity.update(6, true, true), "fresh hover starts after reopening");
    require(!activity.update(6.1, true, true, false), "removing the headset closes audio immediately");
    require(!activity.update(7, true, true), "waking does not inherit stale pointer presence");
    activity.pointer(4, true, 8);
    require(activity.update(8, true, true), "a fresh pointer after waking prewarms audio");
    require(!activity.update(8.1, false, true), "hide also clears an inside pointer");
    require(!activity.has_pointers() && !activity.update(9, true, true),
            "show waits for a new pointer rather than a pre-hide hover");
}
void pointer_surfaces_and_cancellation() {
    KeySoundActivity activity;
    activity.pointer(0, true, 1); // Main overlay.
    require(activity.update(1, true, true), "main surface starts audio");
    activity.pointer(1, true, 2); // The same hand crosses onto its corner overlay.
    activity.pointer(0, false, 2);
    require(activity.update(10, true, true), "leaving one surface cannot clear another");
    require(activity.contains(1) && !activity.contains(0), "surface tokens stay independent");
    activity.clear_pointers(10);
    activity.clear_pointers(11);
    require(activity.update(11.9, true, true), "tracking/focus loss gets the departure grace");
    require(!activity.update(12, true, true), "repeated clears cannot postpone cleanup");
    activity.pointer(5, true, 13);
    require(activity.update(13, true, true), "pointer can return after tracking recovery");
    activity.reset();
    require(!activity.has_pointers() && !activity.update(13.1, true, true),
            "an input-mode reset cannot inherit stale hover or grace");
}
} // namespace
int main() {
    try {
        hover_and_departure();
        suppression_and_reopening();
        pointer_surfaces_and_cancellation();
        std::cout << "Pointer-controlled sound activity passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
