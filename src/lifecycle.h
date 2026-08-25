#pragma once
//
// Lilac2LilyMod lifecycle entry.
//
// This file owns the four top-level hooks:
//   ClientRestart, BackToTitleConfirm, OnExitConfirm, UnrealWindowClose.
//

enum RtStage : int {
    RT_STAGE_COLD = 0,
    RT_STAGE_ARMED = 1,
    RT_STAGE_SESSION = 2,
    RT_STAGE_SHUTDOWN = 3
};

using RtCleanupFn = void (*)();

bool rt_on_unreal_init(const wchar_t* dll_dir) noexcept;
void rt_shutdown() noexcept;
RtStage rt_stage() noexcept;
bool rt_is_in_session() noexcept;
bool rt_is_shutting_down() noexcept;
void rt_add_cleanup(RtCleanupFn fn) noexcept;
