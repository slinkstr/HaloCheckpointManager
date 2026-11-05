configure_file(HCMInternal.rc.in "${CMAKE_CURRENT_BINARY_DIR}/HCMInternal.rc" @ONLY)

add_library(HCMInternal SHARED
    AIFreeze.cpp
    AbilityMeterOverlay.cpp
    AdvanceTicks.cpp
    BSPChangeHookEvent.cpp
    BSPSetChangeHookEvent.cpp
    BinarySetting.cpp
    BlockGameInput.cpp
    BlockPlayerCharacterInput.cpp
    BottomlessClip.cpp
    CarrierBumpAnalyser.cpp
    ChangeOOBBackground.cpp
    CheckpointInjectionLogger.cpp
    CommandConsoleGUI.cpp
    CommandConsoleManager.cpp
    Conduit_ITCFont.cpp
    D3D11Hook.cpp
    DisableBarriers.cpp
    DisableFog.cpp
    DisableScreenEffects.cpp
    DisplayPlayerInfo.cpp
    DynamicStruct.cpp
    DynamicStructFactory.cpp
    EditPlayerViewAngle.cpp
    EditPlayerViewAngleID.cpp
    ForceFutureCheckpoint.cpp
    ForceLaunch.cpp
    ForceTeleport.cpp
    FreeCamera.cpp
    FreeCameraFOVOverride.cpp
    FreeMCCCursor.cpp
    GUIElementConstructor.cpp
    GUIRequiredServices.cpp
    GUIServiceInfo.cpp
    GameEngineDetail.cpp
    GameEngineFunctions.cpp
    GameTickEventHook.cpp
    GetAbilityData.cpp
    GetActiveStructureDesignTags.cpp
    GetAggroData.cpp
    GetBipedsVehicleDatum.cpp
    GetCurrentBSP.cpp
    GetCurrentBSPSet.cpp
    GetCurrentDifficulty.cpp
    GetCurrentLevelCode.cpp
    GetCurrentRNG.cpp
    GetCurrentZoneSet.cpp
    GetDebugString.cpp
    GetGameCameraData.cpp
    GetHCMVersion.cpp
    GetHavokAnchorPoint.cpp
    GetHavokComponent.cpp
    GetMCCVersion.cpp
    GetNextObjectDatum.cpp
    GetObjectAddress.cpp
    GetObjectHealth.cpp
    GetObjectPhysics.cpp
    GetObjectTagName.cpp
    GetPlayerDatum.cpp
    GetPlayerTriggerPosition.cpp
    GetPlayerViewAngle.cpp
    GetScenarioAddress.cpp
    GetSoftCeilingData.cpp
    GetTagName.cpp
    GetTriggerData.cpp
    HCMInternalGUI.cpp
    HCM_imconfig.cpp
    HaloScriptOutputHookEvent.cpp
    HeartbeatTimer.cpp
    HideHUD.cpp
    HotkeyManager.cpp
    HotkeyRebindDialog.cpp
    HotkeyRendererImpl.cpp
    ImGuiManager.cpp
    InfiniteAmmo.cpp
    Invulnerability.cpp
    Lapua.cpp
    LinearSmoother.cpp
    Logging.cpp
    MCCInitialisationCheck.cpp
    MCCStateHook.cpp
    MeasurePlayerDistanceToObject.cpp
    Medusa.cpp
    MessagesGUI.cpp
    MidhookContextInterpreter.cpp
    ModalDialogFactory.cpp
    ModalDialogRenderer.cpp
    ModuleCache.cpp
    ModuleHook.cpp
    ModuleHookManager.cpp
    MultilevelPointer.cpp
    NaturalCheckpointDisable.cpp
    ObjectTableRange.cpp
    OptionalCheatManager.cpp
    PauseGame.cpp
    PlayerPositionToClipboard.cpp
    PointerDataGetter.cpp
    PointerDataParser.cpp
    PointerDataStore.cpp
    ProggyVectorRegularFont.cpp
    Render3DEventProvider.cpp
    RenderTextHelper.cpp
    Renderer2D.cpp
    Renderer3DImpl.cpp
    Renderer3DImpl_Geometry.cpp
    Renderer3DImpl_Sprites.cpp
    Renderer3DImpl_WorldToScreen.cpp
    Renderer3DImpl_updateCameraData.cpp
    RevertEventHook.cpp
    RuntimeExceptionHandler.cpp
    ScopedImFontScaler.cpp
    SensDriftOverlay.cpp
    SetPlayerHealth.cpp
    SettingsEnum.cpp
    SettingsSerialiser.cpp
    SharedMemoryInternal.cpp
    ShieldInputPrinter.cpp
    SkullToggler.cpp
    SoftCeilingOverlay.cpp
    SoundClassGain.cpp
    Speedhack.cpp
    SubpixelID.cpp
    SwitchBSP.cpp
    SwitchBSPSet.cpp
    TagBlockReader.cpp
    TagReferenceReader.cpp
    TagTableRange.cpp
    TextureFactory.cpp
    ThirdPersonRendering.cpp
    TrackTriggerEnterExit.cpp
    TriggerFilter.cpp
    TriggerModel.cpp
    TriggerOverlay.cpp
    UnarySetting.cpp
    UnfreezeObjectPhysics.cpp
    UnhandledExceptionHandler.cpp
    UpdateGameCameraData.cpp
    UpdateTriggerLastChecked.cpp
    UserCameraInputReader.cpp
    ViewAngle3D.cpp
    ViewAngleLineList.cpp
    Waypoint3D.cpp
    WaypointList.cpp
    WindowsUtilities.cpp
    ZoneSetChangeHookEvent.cpp
    dllmain.cpp
    includes/ImGuiColorTextEdit/ImGuiDebugPanel.cpp
    includes/ImGuiColorTextEdit/LanguageDefinitions.cpp
    includes/ImGuiColorTextEdit/TextEditor.cpp
    includes/ImGuiColorTextEdit/UnitTests.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/HCMInternal.rc"
)

target_precompile_headers(HCMInternal PRIVATE pch.h)
set_source_files_properties(
    includes/ImGuiColorTextEdit/ImGuiDebugPanel.cpp
    includes/ImGuiColorTextEdit/LanguageDefinitions.cpp
    includes/ImGuiColorTextEdit/TextEditor.cpp
    includes/ImGuiColorTextEdit/UnitTests.cpp
    PROPERTIES SKIP_PRECOMPILED_HEADERS ON
)

target_compile_options(HCMInternal PRIVATE
    /W3
    /sdl
    /EHsc
    /permissive-
    /std:c++latest
    /wd4005
    $<$<CONFIG:Debug>:/bigobj>
)

target_compile_definitions(HCMInternal PRIVATE
    _WINDOWS
    _USRDLL
    UNICODE
    _UNICODE
    HCMINTERNAL_EXPORTS
    _SILENCE_CXX23_ALIGNED_STORAGE_DEPRECATION_WARNING
    $<$<CONFIG:Debug>:HCM_DEBUG>
)

target_include_directories(HCMInternal PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    includes
    includes/type_safe/include
)

find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(zydis CONFIG REQUIRED)
find_package(pugixml CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(directxtk CONFIG REQUIRED)
find_package(eventpp CONFIG REQUIRED)
find_package(magic_enum CONFIG REQUIRED)
find_package(plog CONFIG REQUIRED)
find_package(CsLibGuarded CONFIG REQUIRED)
find_package(boost_interprocess CONFIG REQUIRED)
find_package(boost_iostreams CONFIG REQUIRED)
find_package(boost_algorithm CONFIG REQUIRED)
find_package(boost_assign CONFIG REQUIRED)
find_package(boost_bimap CONFIG REQUIRED)
find_package(boost_scope_exit CONFIG REQUIRED)
find_package(boost_stacktrace_basic CONFIG REQUIRED)

target_link_libraries(HCMInternal PRIVATE
    safetyhook::safetyhook
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
    Zydis::Zydis
    pugixml::pugixml
    imgui::imgui
    Microsoft::DirectXTK
    eventpp::eventpp
    magic_enum::magic_enum
    plog::plog
    CsLibGuarded::CsLibGuarded
    Boost::interprocess
    Boost::iostreams
    Boost::algorithm
    Boost::assign
    Boost::bimap
    Boost::scope_exit
    Boost::stacktrace_basic
    d3d11
    dxguid
    version
    ws2_32
    wldap32
    crypt32
)

add_custom_command(TARGET HCMInternal POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/InternalPointerData.xml"
        "$<TARGET_FILE_DIR:HCMInternal>/InternalPointerData.xml"
)

set_target_properties(HCMInternal PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${HCM_OUTPUT_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)
