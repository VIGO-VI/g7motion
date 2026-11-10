// SPDX-License-Identifier: GPL-3.0-or-later
// Tiny SDL3 consumer: proves a gamepad is exposed with SDL_SENSOR_GYRO /
// SDL_SENSOR_ACCEL.  Run it while g7motion is active.
#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>

int main() {
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    std::printf("SDL3 %d, %d gamepad(s)\n", SDL_GetVersion(), count);

    for (int i = 0; i < count; ++i) {
        SDL_Gamepad* g = SDL_OpenGamepad(ids[i]);
        if (!g) continue;
        const char* name = SDL_GetGamepadName(g);
        std::printf("  [%d] %s  gyro=%d accel=%d\n", i, name ? name : "?",
                    SDL_GamepadHasSensor(g, SDL_SENSOR_GYRO),
                    SDL_GamepadHasSensor(g, SDL_SENSOR_ACCEL));
        if (SDL_GamepadHasSensor(g, SDL_SENSOR_GYRO)) {
            SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_GYRO, true);
        }
        if (SDL_GamepadHasSensor(g, SDL_SENSOR_ACCEL)) {
            SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_ACCEL, true);
        }
    }
    SDL_free(ids);

    if (count == 0) {
        std::printf("no gamepads\n");
        SDL_Quit();
        return 1;
    }

    const Uint64 end = SDL_GetTicks() + 8000;
    float last_gyro[3] = {0, 0, 0};
    float last_accel[3] = {0, 0, 0};

    while (SDL_GetTicks() < end) {
        SDL_UpdateGamepads();
        SDL_PumpEvents();

        ids = SDL_GetGamepads(&count);
        for (int i = 0; i < count; ++i) {
            SDL_Gamepad* g = SDL_OpenGamepad(ids[i]);
            if (!g) continue;

            float gyro[3] = {0, 0, 0};
            float accel[3] = {0, 0, 0};
            const bool hg = SDL_GetGamepadSensorData(g, SDL_SENSOR_GYRO, gyro, 3);
            const bool ha = SDL_GetGamepadSensorData(g, SDL_SENSOR_ACCEL, accel, 3);

            const bool changed =
                std::fabs(gyro[0] - last_gyro[0]) > 0.005f ||
                std::fabs(gyro[1] - last_gyro[1]) > 0.005f ||
                std::fabs(gyro[2] - last_gyro[2]) > 0.005f ||
                std::fabs(accel[0] - last_accel[0]) > 0.05f ||
                std::fabs(accel[1] - last_accel[1]) > 0.05f ||
                std::fabs(accel[2] - last_accel[2]) > 0.05f;

            if (changed) {
                std::printf("gyro %s %8.4f %8.4f %8.4f rad/s   accel %s %8.4f %8.4f %8.4f m/s^2\n",
                            hg ? "ok" : "no", gyro[0], gyro[1], gyro[2],
                            ha ? "ok" : "no", accel[0], accel[1], accel[2]);
                for (int k = 0; k < 3; ++k) {
                    last_gyro[k] = gyro[k];
                    last_accel[k] = accel[k];
                }
            }
            SDL_CloseGamepad(g);
        }
        SDL_free(ids);
        SDL_Delay(20);
    }

    SDL_Quit();
    return 0;
}
