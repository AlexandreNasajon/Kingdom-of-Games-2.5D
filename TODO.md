# TODO.md - Mouse + Controller Input Implementation Progress

## Plan Breakdown & Progress
1. [x] Add controller globals - g_gamepadConnected, dpad, buttons (A/B/Start).
2. [x] UpdateController() function - Poll gamepad.
3. [x] Fix compile errors - Duplicates, syntax (parens/enums).
4. [ ] Add UpdateOverworld() - WASD/DPAD move, Space/Start menu, Enter/A interact NPC.
5. [x] Mouse for NPC dialog - Click options, hover highlight.
6. [x] Controller in dialog/shop/menu - DPAD nav, A confirm.
7. [ ] UniversalInput() - Global Space menu, Esc close.
8. [ ] Call sites - Main loop/scene updates call UpdateController/UniversalInput.
9. [ ] Test NPC interact distance, menu open, movement.
10. [ ] Full test all scenes, compile/run exe.

**Current Progress:** Input framework (controller, dialog mouse) complete. Compile OK. Missing overworld movement/menu toggle/interact - next.

**Next Step:** Implement UpdateOverworld + call sites.
