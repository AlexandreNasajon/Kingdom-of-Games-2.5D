#include "input.h"

void UpdateOverworld(float dt) {
    UpdateController(dt);

    // Player movement - WASD or D-pad
    float inputX = (float)(IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT) || g_dpadX > 0.5f) - (float)(IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT) || g_dpadX < -0.5f);
    float inputY = (float)(IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN) || g_dpadY > 0.5f) - (float)(IsKeyDown(KEY_W) || IsKeyDown(KEY_UP) || g_dpadY < -0.5f);

    if (fabsf(inputX) > 0.5f || fabsf(inputY) > 0.5f) {
        int newGX = g_player.gridX + (inputX > 0 ? 1 : (inputX < 0 ? -1 : 0));
        int newGY = g_player.gridY + (inputY > 0 ? 1 : (inputY < 0 ? -1 : 0));

        if (newGX >= 0 && newGX < MAP_W && newGY >= 0 && newGY < MAP_H && g_collision[newGY][newGX] == 0) {
            g_player.targetX = newGX;
            g_player.targetY = newGY;
            g_player.walking = true;
            g_player.walkT = 0.0f;
            if (fabsf(inputX) > fabsf(inputY)) g_player.dir = inputX > 0 ? DIR_RIGHT : DIR_LEFT;
            else g_player.dir = inputY > 0 ? DIR_DOWN : DIR_UP;
        }
    }

    if (g_player.walking) {
        g_player.walkT += MOVE_SPEED * dt;
        g_player.visualX = Lerp(g_player.visualX, (float)g_player.targetX, 0.2f);
        g_player.visualZ = Lerp(g_player.visualZ, (float)g_player.targetY, 0.2f);
        if (g_player.walkT > 1.0f) {
            g_player.walking = false;
            g_player.gridX = g_player.targetX;
            g_player.gridY = g_player.targetY;
        }
    }

    // Enter/A interact
    if (IsKeyPressed(KEY_ENTER) || g_padAPressed) {
        // NPC
        for (int i = 0; i < 4; i++) {
            float dx = g_npcs[i].worldX - g_player.visualX;
            float dz = g_npcs[i].worldZ - g_player.visualZ;
            if (sqrtf(dx*dx + dz*dz) < 1.5f) {
                g_npcDialogOpen = true;
                g_targetNPC = i;
                g_dialogSelection = 0;
                break;
            }
        }
        // Triggers
        for (auto& tz : g_triggerZones) {
            if (g_player.visualX >= tz.minX && g_player.visualX <= tz.maxX &&
                g_player.visualZ >= tz.minZ && g_player.visualZ <= tz.maxZ &&
                g_player.dir == tz.requiredDir) {
                g_scene = tz.targetScene;
                g_player.gridX = tz.destGridX;
                g_player.gridY = tz.destGridY;
                g_player.dir = tz.destDir;
                g_savedGridX = g_player.gridX;
                g_savedGridY = g_player.gridY;
                g_savedDir = g_player.dir;
                break;
            }
        }
    }
}

static void UniversalInput(float dt) {
    // Space / Start: Toggle menu in overworld
    if ((IsKeyPressed(KEY_SPACE) || g_padStartPressed) && g_scene == SCENE_OVERWORLD) {
        g_menuOpen = !g_menuOpen;
    }

    // Esc / B: Close
    if (IsKeyPressed(KEY_ESCAPE) || g_padBPressed) {
        g_menuOpen = false;
        g_npcDialogOpen = false;
        if (g_scene == SCENE_SHOP) g_scene = SCENE_TENT3;
    }
}
