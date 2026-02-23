#pragma once

/*namespace offsets
{
    constexpr auto iViewMatrix = 0x17DFD0;
    constexpr auto iLocalPlayer = 0x0018AC00;
    constexpr auto iEntityList = 0x00191FCC;

    constexpr auto vHead = 0x4;
    constexpr auto iTeam = 0x30C;
    constexpr auto isDead = 0x0318;
    constexpr auto pYaw = 0x34;
    constexpr auto vFeet = 0x28;
    constexpr auto pHealth = 0xEC;
    constexpr auto pPitch = 0x38;
}*/


namespace Offsets {
    namespace ModBase {
        constexpr uintptr_t World = 0xF4B050;
        constexpr uintptr_t CameraActive = 0xF59A18;
        constexpr uintptr_t CameraMode = 0xF59988; // 3 = Debug/Freecam

        // Debug Camera
        constexpr uintptr_t FreeDebugCamera_GetInstance = 0x483390;
        constexpr uintptr_t Global_FreeDebugCameraInstance = 0xF29EF8;

        // Position & Rotation Globals
        constexpr uintptr_t DebugCameraTargetPos = 0xF599F0;
        constexpr uintptr_t DebugCameraStartPos = 0xF599B4;
        constexpr uintptr_t RotationSource1 = 0xF599D0; // Row 0 (Right), Row 1 at +0x10 (Up)
        constexpr uintptr_t RotationSource2 = 0xF59994; // Alternate source

        // Patches
        constexpr uintptr_t CameraUpdatePatch = 0x87C034;
        constexpr uintptr_t InputHandlerPatch = 0x482420;
    }
    namespace World {
        constexpr uintptr_t LocalPlayer = 0xF70840;
    }
}

/*
if( settings::speedhack )
        {
            if( inputsystem::down( settings::speedhack_key ) )
            {
                //auto lol = chab::m_driver->read< std::uintptr_t >( chab::m_base_address + 0xF193C8 );
                //printf( "lol: %llx\n", lol );

                chab::m_driver->write< std::uintptr_t >( chab::m_base_address + offsets::m_speed, 0x980000 );
                utils::sleep( 2 );
                chab::m_driver->write< std::uintptr_t >( chab::m_base_address + offsets::m_speed, 0x989680 );
                utils::sleep( 2 );
                chab::m_driver->write< std::uintptr_t >( chab::m_base_address + offsets::m_speed, 0x980000 );
            }
            else
            {
                chab::m_driver->write< std::uintptr_t >( chab::m_base_address + offsets::m_speed, 0x989680 );
            }
        }
*/

/*
if (settings::speedhack)
{
    if (inputsystem::down(settings::speedhack_key))
    {
        // +5% speed
        write(speed, 0xA037A0);
    }
    else
    {
        // normal speed
        write(speed, 0x989680);
    }
}
*/