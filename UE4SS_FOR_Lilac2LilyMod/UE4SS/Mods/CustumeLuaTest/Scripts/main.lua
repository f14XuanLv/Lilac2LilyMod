local MOD = "[CustumeLuaTest]"

local FN_CLIENT_RESTART = "/Script/Engine.PlayerController:ClientRestart"
local FN_BACK_TO_TITLE_CONFIRM = "/Game/_Zion/UI/Pause/WBP_Pause.WBP_Pause_C:OnBackToTitleConfirm"
local FN_EXIT_CONFIRM = "/Game/_Zion/UI/Pause/WBP_Pause.WBP_Pause_C:OnExitConfirm"

local secondary_hooks_registered = false
local client_pre_id, client_post_id
local back_pre_id, back_post_id
local exit_pre_id, exit_post_id

local function log(message)
    print(string.format("%s %s\n", MOD, message))
end

local function on_back_to_title_confirm()
    log("BackToTitleConfirm triggered")
end

local function on_exit_confirm()
    log("ExitConfirm triggered")
end

local function on_client_restart()
    log("ClientRestart triggered")

    if secondary_hooks_registered then
        return
    end

    back_pre_id, back_post_id = RegisterHook(FN_BACK_TO_TITLE_CONFIRM, on_back_to_title_confirm)
    log(string.format("BackToTitleConfirm registered pre=%s post=%s", tostring(back_pre_id), tostring(back_post_id)))

    exit_pre_id, exit_post_id = RegisterHook(FN_EXIT_CONFIRM, on_exit_confirm)
    log(string.format("ExitConfirm registered pre=%s post=%s", tostring(exit_pre_id), tostring(exit_post_id)))

    secondary_hooks_registered = true
end

client_pre_id, client_post_id = RegisterHook(FN_CLIENT_RESTART, on_client_restart)
log(string.format("ClientRestart registered pre=%s post=%s", tostring(client_pre_id), tostring(client_post_id)))
