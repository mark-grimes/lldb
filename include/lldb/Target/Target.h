//===-- Target.h ------------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_Target_h_
#define liblldb_Target_h_

// C Includes
// C++ Includes
#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Other libraries and framework includes
// Project includes
#include "lldb/lldb-public.h"
#include "lldb/Breakpoint/BreakpointList.h"
#include "lldb/Breakpoint/WatchpointList.h"
#include "lldb/Core/ArchSpec.h"
#include "lldb/Core/Broadcaster.h"
#include "lldb/Core/Disassembler.h"
#include "lldb/Core/ModuleList.h"
#include "lldb/Core/UserSettingsController.h"
#include "lldb/Expression/Expression.h"
#include "lldb/Symbol/TypeSystem.h"
#include "lldb/Target/ExecutionContextScope.h"
#include "lldb/Target/PathMappingList.h"
#include "lldb/Target/ProcessLaunchInfo.h"
#include "lldb/Target/SectionLoadHistory.h"

namespace lldb_private {

extern OptionEnumValueElement g_dynamic_value_types[];

typedef enum InlineStrategy
{
    eInlineBreakpointsNever = 0,
    eInlineBreakpointsHeaders,
    eInlineBreakpointsAlways
} InlineStrategy;

typedef enum LoadScriptFromSymFile
{
    eLoadScriptFromSymFileTrue,
    eLoadScriptFromSymFileFalse,
    eLoadScriptFromSymFileWarn
} LoadScriptFromSymFile;

typedef enum LoadCWDlldbinitFile
{
    eLoadCWDlldbinitTrue,
    eLoadCWDlldbinitFalse,
    eLoadCWDlldbinitWarn
} LoadCWDlldbinitFile;

//----------------------------------------------------------------------
// TargetProperties
//----------------------------------------------------------------------
class TargetProperties : public Properties
{
public:
    TargetProperties(Target *target);

    ~TargetProperties() override;
    
    ArchSpec
    GetDefaultArchitecture () const;
    
    void
    SetDefaultArchitecture (const ArchSpec& arch);

    bool
    GetMoveToNearestCode () const;

    lldb::DynamicValueType
    GetPreferDynamicValue() const;

    bool
    SetPreferDynamicValue (lldb::DynamicValueType d);

    bool
    GetDisableASLR () const;
    
    void
    SetDisableASLR (bool b);
    
    bool
    GetDetachOnError () const;
    
    void
    SetDetachOnError (bool b);
    
    bool
    GetDisableSTDIO () const;
    
    void
    SetDisableSTDIO (bool b);
    
    const char *
    GetDisassemblyFlavor() const;

//    void
//    SetDisassemblyFlavor(const char *flavor);
    
    InlineStrategy
    GetInlineStrategy () const;

    const char *
    GetArg0 () const;
    
    void
    SetArg0 (const char *arg);

    bool
    GetRunArguments (Args &args) const;
    
    void
    SetRunArguments (const Args &args);
    
    size_t
    GetEnvironmentAsArgs (Args &env) const;

    void
    SetEnvironmentFromArgs (const Args &env);

    bool
    GetSkipPrologue() const;
    
    PathMappingList &
    GetSourcePathMap () const;
    
    FileSpecList &
    GetExecutableSearchPaths ();

    FileSpecList &
    GetDebugFileSearchPaths ();
    
    FileSpecList &
    GetClangModuleSearchPaths ();
    
    bool
    GetEnableAutoImportClangModules () const;
    
    bool
    GetEnableSyntheticValue () const;
    
    uint32_t
    GetMaximumNumberOfChildrenToDisplay() const;
    
    uint32_t
    GetMaximumSizeOfStringSummary() const;

    uint32_t
    GetMaximumMemReadSize () const;
    
    FileSpec
    GetStandardInputPath () const;
    
    void
    SetStandardInputPath (const char *path);
    
    FileSpec
    GetStandardOutputPath () const;
    
    void
    SetStandardOutputPath (const char *path);
    
    FileSpec
    GetStandardErrorPath () const;
    
    void
    SetStandardErrorPath (const char *path);
    
    bool
    GetBreakpointsConsultPlatformAvoidList ();
    
    lldb::LanguageType
    GetLanguage () const;

    const char *
    GetExpressionPrefixContentsAsCString ();

    bool
    GetUseHexImmediates() const;

    bool
    GetUseFastStepping() const;
    
    bool
    GetDisplayExpressionsInCrashlogs () const;

    LoadScriptFromSymFile
    GetLoadScriptFromSymbolFile() const;

    LoadCWDlldbinitFile
    GetLoadCWDlldbinitFile () const;

    Disassembler::HexImmediateStyle
    GetHexImmediateStyle() const;
    
    MemoryModuleLoadLevel
    GetMemoryModuleLoadLevel() const;

    bool
    GetUserSpecifiedTrapHandlerNames (Args &args) const;

    void
    SetUserSpecifiedTrapHandlerNames (const Args &args);

    bool
    GetNonStopModeEnabled () const;

    void
    SetNonStopModeEnabled (bool b);

    bool
    GetDisplayRuntimeSupportValues () const;
    
    void
    SetDisplayRuntimeSupportValues (bool b);

    const ProcessLaunchInfo &
    GetProcessLaunchInfo();

    void
    SetProcessLaunchInfo(const ProcessLaunchInfo &launch_info);

private:
    //------------------------------------------------------------------
    // Callbacks for m_launch_info.
    //------------------------------------------------------------------
    static void Arg0ValueChangedCallback(void *target_property_ptr, OptionValue *);
    static void RunArgsValueChangedCallback(void *target_property_ptr, OptionValue *);
    static void EnvVarsValueChangedCallback(void *target_property_ptr, OptionValue *);
    static void InheritEnvValueChangedCallback(void *target_property_ptr, OptionValue *);
    static void InputPathValueChangedCallback(void *target_property_ptr, OptionValue *);
    static void OutputPathValueChangedCallback(void *target_property_ptr, OptionValue *);
    static void ErrorPathValueChangedCallback(void *target_property_ptr, OptionValue *);
    static void DetachOnErrorValueChangedCallback(void *target_property_ptr, OptionValue *);
    static void DisableASLRValueChangedCallback(void *target_property_ptr, OptionValue *);
    static void DisableSTDIOValueChangedCallback(void *target_property_ptr, OptionValue *);

    //------------------------------------------------------------------
    // Member variables.
    //------------------------------------------------------------------
    ProcessLaunchInfo m_launch_info;
};

class EvaluateExpressionOptions
{
public:
    static const uint32_t default_timeout = 500000;
    EvaluateExpressionOptions() :
        m_execution_policy(eExecutionPolicyOnlyWhenNeeded),
        m_language (lldb::eLanguageTypeUnknown),
        m_prefix (), // A prefix specific to this expression that is added after the prefix from the settings (if any)
        m_coerce_to_id (false),
        m_unwind_on_error (true),
        m_ignore_breakpoints (false),
        m_keep_in_memory (false),
        m_try_others (true),
        m_stop_others (true),
        m_debug (false),
        m_trap_exceptions (true),
        m_generate_debug_info (false),
        m_result_is_internal (false),
        m_use_dynamic (lldb::eNoDynamicValues),
        m_timeout_usec (default_timeout),
        m_one_thread_timeout_usec (0),
        m_cancel_callback (nullptr),
        m_cancel_callback_baton (nullptr)
    {
    }
    
    ExecutionPolicy
    GetExecutionPolicy () const
    {
        return m_execution_policy;
    }
    
    void
    SetExecutionPolicy (ExecutionPolicy policy = eExecutionPolicyAlways)
    {
        m_execution_policy = policy;
    }
    
    lldb::LanguageType
    GetLanguage() const
    {
        return m_language;
    }
    
    void
    SetLanguage(lldb::LanguageType language)
    {
        m_language = language;
    }
    
    bool
    DoesCoerceToId () const
    {
        return m_coerce_to_id;
    }

    const char *
    GetPrefix () const
    {
        return (m_prefix.empty() ? nullptr : m_prefix.c_str());
    }

    void
    SetPrefix (const char *prefix)
    {
        if (prefix && prefix[0])
            m_prefix = prefix;
        else
            m_prefix.clear();
    }

    void
    SetCoerceToId (bool coerce = true)
    {
        m_coerce_to_id = coerce;
    }
    
    bool
    DoesUnwindOnError () const
    {
        return m_unwind_on_error;
    }
    
    void
    SetUnwindOnError (bool unwind = false)
    {
        m_unwind_on_error = unwind;
    }
    
    bool
    DoesIgnoreBreakpoints () const
    {
        return m_ignore_breakpoints;
    }
    
    void
    SetIgnoreBreakpoints (bool ignore = false)
    {
        m_ignore_breakpoints = ignore;
    }
    
    bool
    DoesKeepInMemory () const
    {
        return m_keep_in_memory;
    }
    
    void
    SetKeepInMemory (bool keep = true)
    {
        m_keep_in_memory = keep;
    }
    
    lldb::DynamicValueType
    GetUseDynamic () const
    {
        return m_use_dynamic;
    }
    
    void
    SetUseDynamic (lldb::DynamicValueType dynamic = lldb::eDynamicCanRunTarget)
    {
        m_use_dynamic = dynamic;
    }
    
    uint32_t
    GetTimeoutUsec () const
    {
        return m_timeout_usec;
    }
    
    void
    SetTimeoutUsec (uint32_t timeout = 0)
    {
        m_timeout_usec = timeout;
    }
    
    uint32_t
    GetOneThreadTimeoutUsec () const
    {
        return m_one_thread_timeout_usec;
    }
    
    void
    SetOneThreadTimeoutUsec (uint32_t timeout = 0)
    {
        m_one_thread_timeout_usec = timeout;
    }
    
    bool
    GetTryAllThreads () const
    {
        return m_try_others;
    }
    
    void
    SetTryAllThreads (bool try_others = true)
    {
        m_try_others = try_others;
    }
    
    bool
    GetStopOthers () const
    {
        return m_stop_others;
    }
    
    void
    SetStopOthers (bool stop_others = true)
    {
        m_stop_others = stop_others;
    }
    
    bool
    GetDebug() const
    {
        return m_debug;
    }
    
    void
    SetDebug(bool b)
    {
        m_debug = b;
        if (m_debug)
            m_generate_debug_info = true;
    }
    
    bool
    GetGenerateDebugInfo() const
    {
        return m_generate_debug_info;
    }
    
    void
    SetGenerateDebugInfo(bool b)
    {
        m_generate_debug_info = b;
    }
    
    bool
    GetColorizeErrors () const
    {
        return m_ansi_color_errors;
    }
    
    void
    SetColorizeErrors (bool b)
    {
        m_ansi_color_errors = b;
    }
    
    bool
    GetTrapExceptions() const
    {
        return m_trap_exceptions;
    }
    
    void
    SetTrapExceptions (bool b)
    {
        m_trap_exceptions = b;
    }
    
    bool
    GetREPLEnabled() const
    {
        return m_repl;
    }
    
    void
    SetREPLEnabled (bool b)
    {
        m_repl = b;
    }
    
    void
    SetCancelCallback (lldb::ExpressionCancelCallback callback, void *baton)
    {
        m_cancel_callback_baton = baton;
        m_cancel_callback = callback;
    }
    
    bool
    InvokeCancelCallback (lldb::ExpressionEvaluationPhase phase) const
    {
        return ((m_cancel_callback != nullptr) ? m_cancel_callback(phase, m_cancel_callback_baton) : false);
    }
    
    // Allows the expression contents to be remapped to point to the specified file and line
    // using #line directives.
    void
    SetPoundLine (const char *path, uint32_t line) const
    {
        if (path && path[0])
        {
            m_pound_line_file = path;
            m_pound_line_line = line;
        }
        else
        {
            m_pound_line_file.clear();
            m_pound_line_line = 0;
        }
    }
    
    const char *
    GetPoundLineFilePath () const
    {
        return (m_pound_line_file.empty() ? nullptr : m_pound_line_file.c_str());
    }
    
    uint32_t
    GetPoundLineLine () const
    {
        return m_pound_line_line;
    }

    void
    SetResultIsInternal (bool b)
    {
        m_result_is_internal = b;
    }

    bool
    GetResultIsInternal () const
    {
        return m_result_is_internal;
    }

private:
    ExecutionPolicy m_execution_policy;
    lldb::LanguageType m_language;
    std::string m_prefix;
    bool m_coerce_to_id;
    bool m_unwind_on_error;
    bool m_ignore_breakpoints;
    bool m_keep_in_memory;
    bool m_try_others;
    bool m_stop_others;
    bool m_debug;
    bool m_trap_exceptions;
    bool m_repl;
    bool m_generate_debug_info;
    bool m_ansi_color_errors;
    bool m_result_is_internal;
    lldb::DynamicValueType m_use_dynamic;
    uint32_t m_timeout_usec;
    uint32_t m_one_thread_timeout_usec;
    lldb::ExpressionCancelCallback m_cancel_callback;
    void *m_cancel_callback_baton;
    // If m_pound_line_file is not empty and m_pound_line_line is non-zero,
    // use #line %u "%s" before the expression content to remap where the source
    // originates
    mutable std::string m_pound_line_file;
    mutable uint32_t m_pound_line_line;
};

//----------------------------------------------------------------------
// Target
//----------------------------------------------------------------------
class Target :
    public std::enable_shared_from_this<Target>,
    public TargetProperties,
    public Broadcaster,
    public ExecutionContextScope,
    public ModuleList::Notifier
{
public:
    friend class TargetList;

    //------------------------------------------------------------------
    /// Broadcaster event bits definitions.
    //------------------------------------------------------------------
    enum
    {
        eBroadcastBitBreakpointChanged  = (1 << 0),
        eBroadcastBitModulesLoaded      = (1 << 1),
        eBroadcastBitModulesUnloaded    = (1 << 2),
        eBroadcastBitWatchpointChanged  = (1 << 3),
        eBroadcastBitSymbolsLoaded      = (1 << 4)
    };
    
    // These two functions fill out the Broadcaster interface:
    
    static ConstString &GetStaticBroadcasterClass ();

    ConstString &GetBroadcasterClass() const override
    {
        return GetStaticBroadcasterClass();
    }

    // This event data class is for use by the TargetList to broadcast new target notifications.
    class TargetEventData : public EventData
    {
    public:
        TargetEventData (const lldb::TargetSP &target_sp);

        TargetEventData (const lldb::TargetSP &target_sp, const ModuleList &module_list);

        ~TargetEventData() override;

        static const ConstString &
        GetFlavorString ();

        const ConstString &
        GetFlavor() const override
        {
            return TargetEventData::GetFlavorString ();
        }

        void
        Dump(Stream *s) const override;

        static const TargetEventData *
        GetEventDataFromEvent (const Event *event_ptr);

        static lldb::TargetSP
        GetTargetFromEvent (const Event *event_ptr);

        static ModuleList
        GetModuleListFromEvent (const Event *event_ptr);

        const lldb::TargetSP &
        GetTarget() const
        {
            return m_target_sp;
        }

        const ModuleList &
        GetModuleList() const
        {
            return m_module_list;
        }

    private:
        lldb::TargetSP m_target_sp;
        ModuleList     m_module_list;

        DISALLOW_COPY_AND_ASSIGN (TargetEventData);
    };
    
    ~Target() override;

    static void
    SettingsInitialize ();

    static void
    SettingsTerminate ();

    static FileSpecList
    GetDefaultExecutableSearchPaths ();

    static FileSpecList
    GetDefaultDebugFileSearchPaths ();
    
    static FileSpecList
    GetDefaultClangModuleSearchPaths ();

    static ArchSpec
    GetDefaultArchitecture ();

    static void
    SetDefaultArchitecture (const ArchSpec &arch);

//    void
//    UpdateInstanceName ();

    lldb::ModuleSP
    GetSharedModule(const ModuleSpec &module_spec,
                    Error *error_ptr = nullptr);

    //----------------------------------------------------------------------
    // Settings accessors
    //----------------------------------------------------------------------

    static const lldb::TargetPropertiesSP &
    GetGlobalProperties();

    Mutex &
    GetAPIMutex ()
    {
        return m_mutex;
    }

    void
    DeleteCurrentProcess ();

    void
    CleanupProcess ();

    //------------------------------------------------------------------
    /// Dump a description of this object to a Stream.
    ///
    /// Dump a description of the contents of this object to the
    /// supplied stream \a s. The dumped content will be only what has
    /// been loaded or parsed up to this point at which this function
    /// is called, so this is a good way to see what has been parsed
    /// in a target.
    ///
    /// @param[in] s
    ///     The stream to which to dump the object description.
    //------------------------------------------------------------------
    void
    Dump (Stream *s, lldb::DescriptionLevel description_level);

    const lldb::ProcessSP &
    CreateProcess (lldb::ListenerSP listener,
                   const char *plugin_name,
                   const FileSpec *crash_file);

    const lldb::ProcessSP &
    GetProcessSP () const;

    bool
    IsValid()
    {
        return m_valid;
    }

    void
    Destroy();
    
    Error
    Launch (ProcessLaunchInfo &launch_info,
            Stream *stream);  // Optional stream to receive first stop info

    Error
    Attach (ProcessAttachInfo &attach_info,
            Stream *stream);  // Optional stream to receive first stop info

    //------------------------------------------------------------------
    // This part handles the breakpoints.
    //------------------------------------------------------------------

    BreakpointList &
    GetBreakpointList(bool internal = false);

    const BreakpointList &
    GetBreakpointList(bool internal = false) const;
    
    lldb::BreakpointSP
    GetLastCreatedBreakpoint ()
    {
        return m_last_created_breakpoint;
    }

    lldb::BreakpointSP
    GetBreakpointByID (lldb::break_id_t break_id);

    // Use this to create a file and line breakpoint to a given module or all module it is nullptr
    lldb::BreakpointSP
    CreateBreakpoint (const FileSpecList *containingModules,
                      const FileSpec &file,
                      uint32_t line_no,
                      LazyBool check_inlines,
                      LazyBool skip_prologue,
                      bool internal,
                      bool request_hardware,
                      LazyBool move_to_nearest_code);

    // Use this to create breakpoint that matches regex against the source lines in files given in source_file_list:
    lldb::BreakpointSP
    CreateSourceRegexBreakpoint (const FileSpecList *containingModules,
                                 const FileSpecList *source_file_list,
                                 RegularExpression &source_regex,
                                 bool internal,
                                 bool request_hardware,
                                 LazyBool move_to_nearest_code);

    // Use this to create a breakpoint from a load address
    lldb::BreakpointSP
    CreateBreakpoint (lldb::addr_t load_addr,
                      bool internal,
                      bool request_hardware);

    // Use this to create a breakpoint from a load address and a module file spec
    lldb::BreakpointSP
    CreateAddressInModuleBreakpoint (lldb::addr_t file_addr,
                                     bool internal,
                                     const FileSpec *file_spec,
                                     bool request_hardware);

    // Use this to create Address breakpoints:
    lldb::BreakpointSP
    CreateBreakpoint (const Address &addr,
                      bool internal,
                      bool request_hardware);

    // Use this to create a function breakpoint by regexp in containingModule/containingSourceFiles, or all modules if it is nullptr
    // When "skip_prologue is set to eLazyBoolCalculate, we use the current target 
    // setting, else we use the values passed in
    lldb::BreakpointSP
    CreateFuncRegexBreakpoint (const FileSpecList *containingModules,
                               const FileSpecList *containingSourceFiles,
                               RegularExpression &func_regexp,
                               lldb::LanguageType requested_language,
                               LazyBool skip_prologue,
                               bool internal,
                               bool request_hardware);

    // Use this to create a function breakpoint by name in containingModule, or all modules if it is nullptr
    // When "skip_prologue is set to eLazyBoolCalculate, we use the current target 
    // setting, else we use the values passed in.
    // func_name_type_mask is or'ed values from the FunctionNameType enum.
    lldb::BreakpointSP
    CreateBreakpoint (const FileSpecList *containingModules,
                      const FileSpecList *containingSourceFiles,
                      const char *func_name,
                      uint32_t func_name_type_mask, 
                      lldb::LanguageType language,
                      LazyBool skip_prologue,
                      bool internal,
                      bool request_hardware);
                      
    lldb::BreakpointSP
    CreateExceptionBreakpoint(enum lldb::LanguageType language,
                              bool catch_bp,
                              bool throw_bp,
                              bool internal,
                              Args *additional_args = nullptr,
                              Error *additional_args_error = nullptr);
    
    // This is the same as the func_name breakpoint except that you can specify a vector of names.  This is cheaper
    // than a regular expression breakpoint in the case where you just want to set a breakpoint on a set of names
    // you already know.
    // func_name_type_mask is or'ed values from the FunctionNameType enum.
    lldb::BreakpointSP
    CreateBreakpoint (const FileSpecList *containingModules,
                      const FileSpecList *containingSourceFiles,
                      const char *func_names[],
                      size_t num_names, 
                      uint32_t func_name_type_mask, 
                      lldb::LanguageType language,
                      LazyBool skip_prologue,
                      bool internal,
                      bool request_hardware);

    lldb::BreakpointSP
    CreateBreakpoint (const FileSpecList *containingModules,
                      const FileSpecList *containingSourceFiles,
                      const std::vector<std::string> &func_names,
                      uint32_t func_name_type_mask,
                      lldb::LanguageType language,
                      LazyBool skip_prologue,
                      bool internal,
                      bool request_hardware);

    // Use this to create a general breakpoint:
    lldb::BreakpointSP
    CreateBreakpoint (lldb::SearchFilterSP &filter_sp,
                      lldb::BreakpointResolverSP &resolver_sp,
                      bool internal,
                      bool request_hardware,
                      bool resolve_indirect_symbols);

    // Use this to create a watchpoint:
    lldb::WatchpointSP
    CreateWatchpoint (lldb::addr_t addr,
                      size_t size,
                      const CompilerType *type,
                      uint32_t kind,
                      Error &error);

    lldb::WatchpointSP
    GetLastCreatedWatchpoint ()
    {
        return m_last_created_watchpoint;
    }

    WatchpointList &
    GetWatchpointList()
    {
        return m_watchpoint_list;
    }

    void
    RemoveAllBreakpoints (bool internal_also = false);

    void
    DisableAllBreakpoints (bool internal_also = false);

    void
    EnableAllBreakpoints (bool internal_also = false);

    bool
    DisableBreakpointByID (lldb::break_id_t break_id);

    bool
    EnableBreakpointByID (lldb::break_id_t break_id);

    bool
    RemoveBreakpointByID (lldb::break_id_t break_id);

    // The flag 'end_to_end', default to true, signifies that the operation is
    // performed end to end, for both the debugger and the debuggee.

    bool
    RemoveAllWatchpoints (bool end_to_end = true);

    bool
    DisableAllWatchpoints (bool end_to_end = true);

    bool
    EnableAllWatchpoints (bool end_to_end = true);

    bool
    ClearAllWatchpointHitCounts ();

    bool
    ClearAllWatchpointHistoricValues ();
    
    bool
    IgnoreAllWatchpoints (uint32_t ignore_count);

    bool
    DisableWatchpointByID (lldb::watch_id_t watch_id);

    bool
    EnableWatchpointByID (lldb::watch_id_t watch_id);

    bool
    RemoveWatchpointByID (lldb::watch_id_t watch_id);

    bool
    IgnoreWatchpointByID (lldb::watch_id_t watch_id, uint32_t ignore_count);

    //------------------------------------------------------------------
    /// Get \a load_addr as a callable code load address for this target
    ///
    /// Take \a load_addr and potentially add any address bits that are 
    /// needed to make the address callable. For ARM this can set bit
    /// zero (if it already isn't) if \a load_addr is a thumb function.
    /// If \a addr_class is set to eAddressClassInvalid, then the address
    /// adjustment will always happen. If it is set to an address class
    /// that doesn't have code in it, LLDB_INVALID_ADDRESS will be 
    /// returned.
    //------------------------------------------------------------------
    lldb::addr_t
    GetCallableLoadAddress (lldb::addr_t load_addr, lldb::AddressClass addr_class = lldb::eAddressClassInvalid) const;

    //------------------------------------------------------------------
    /// Get \a load_addr as an opcode for this target.
    ///
    /// Take \a load_addr and potentially strip any address bits that are 
    /// needed to make the address point to an opcode. For ARM this can 
    /// clear bit zero (if it already isn't) if \a load_addr is a 
    /// thumb function and load_addr is in code.
    /// If \a addr_class is set to eAddressClassInvalid, then the address
    /// adjustment will always happen. If it is set to an address class
    /// that doesn't have code in it, LLDB_INVALID_ADDRESS will be 
    /// returned.
    //------------------------------------------------------------------
    lldb::addr_t
    GetOpcodeLoadAddress (lldb::addr_t load_addr, lldb::AddressClass addr_class = lldb::eAddressClassInvalid) const;

    // Get load_addr as breakable load address for this target.
    // Take a addr and check if for any reason there is a better address than this to put a breakpoint on.
    // If there is then return that address.
    // For MIPS, if instruction at addr is a delay slot instruction then this method will find the address of its
    // previous instruction and return that address.
    lldb::addr_t
    GetBreakableLoadAddress (lldb::addr_t addr);

    void
    ModulesDidLoad (ModuleList &module_list);

    void
    ModulesDidUnload (ModuleList &module_list, bool delete_locations);
    
    void
    SymbolsDidLoad (ModuleList &module_list);
    
    void
    ClearModules(bool delete_locations);

    //------------------------------------------------------------------
    /// Called as the last function in Process::DidExec().
    ///
    /// Process::DidExec() will clear a lot of state in the process,
    /// then try to reload a dynamic loader plugin to discover what
    /// binaries are currently available and then this function should
    /// be called to allow the target to do any cleanup after everything
    /// has been figured out. It can remove breakpoints that no longer
    /// make sense as the exec might have changed the target
    /// architecture, and unloaded some modules that might get deleted.
    //------------------------------------------------------------------
    void
    DidExec ();
    
    //------------------------------------------------------------------
    /// Gets the module for the main executable.
    ///
    /// Each process has a notion of a main executable that is the file
    /// that will be executed or attached to. Executable files can have
    /// dependent modules that are discovered from the object files, or
    /// discovered at runtime as things are dynamically loaded.
    ///
    /// @return
    ///     The shared pointer to the executable module which can
    ///     contains a nullptr Module object if no executable has been
    ///     set.
    ///
    /// @see DynamicLoader
    /// @see ObjectFile::GetDependentModules (FileSpecList&)
    /// @see Process::SetExecutableModule(lldb::ModuleSP&)
    //------------------------------------------------------------------
    lldb::ModuleSP
    GetExecutableModule ();

    Module*
    GetExecutableModulePointer ();

    //------------------------------------------------------------------
    /// Set the main executable module.
    ///
    /// Each process has a notion of a main executable that is the file
    /// that will be executed or attached to. Executable files can have
    /// dependent modules that are discovered from the object files, or
    /// discovered at runtime as things are dynamically loaded.
    ///
    /// Setting the executable causes any of the current dependant
    /// image information to be cleared and replaced with the static
    /// dependent image information found by calling
    /// ObjectFile::GetDependentModules (FileSpecList&) on the main
    /// executable and any modules on which it depends. Calling
    /// Process::GetImages() will return the newly found images that
    /// were obtained from all of the object files.
    ///
    /// @param[in] module_sp
    ///     A shared pointer reference to the module that will become
    ///     the main executable for this process.
    ///
    /// @param[in] get_dependent_files
    ///     If \b true then ask the object files to track down any
    ///     known dependent files.
    ///
    /// @see ObjectFile::GetDependentModules (FileSpecList&)
    /// @see Process::GetImages()
    //------------------------------------------------------------------
    void
    SetExecutableModule (lldb::ModuleSP& module_sp, bool get_dependent_files);

    bool
    LoadScriptingResources(std::list<Error>& errors,
                           Stream* feedback_stream = nullptr,
                           bool continue_on_error = true)
    {
        return m_images.LoadScriptingResourcesInTarget(this,errors,feedback_stream,continue_on_error);
    }
    
    //------------------------------------------------------------------
    /// Get accessor for the images for this process.
    ///
    /// Each process has a notion of a main executable that is the file
    /// that will be executed or attached to. Executable files can have
    /// dependent modules that are discovered from the object files, or
    /// discovered at runtime as things are dynamically loaded. After
    /// a main executable has been set, the images will contain a list
    /// of all the files that the executable depends upon as far as the
    /// object files know. These images will usually contain valid file
    /// virtual addresses only. When the process is launched or attached
    /// to, the DynamicLoader plug-in will discover where these images
    /// were loaded in memory and will resolve the load virtual
    /// addresses is each image, and also in images that are loaded by
    /// code.
    ///
    /// @return
    ///     A list of Module objects in a module list.
    //------------------------------------------------------------------
    const ModuleList&
    GetImages () const
    {
        return m_images;
    }
    
    ModuleList&
    GetImages ()
    {
        return m_images;
    }
    
    //------------------------------------------------------------------
    /// Return whether this FileSpec corresponds to a module that should be considered for general searches.
    ///
    /// This API will be consulted by the SearchFilterForUnconstrainedSearches
    /// and any module that returns \b true will not be searched.  Note the
    /// SearchFilterForUnconstrainedSearches is the search filter that
    /// gets used in the CreateBreakpoint calls when no modules is provided.
    ///
    /// The target call at present just consults the Platform's call of the
    /// same name.
    /// 
    /// @param[in] module_sp
    ///     A shared pointer reference to the module that checked.
    ///
    /// @return \b true if the module should be excluded, \b false otherwise.
    //------------------------------------------------------------------
    bool
    ModuleIsExcludedForUnconstrainedSearches (const FileSpec &module_spec);
    
    //------------------------------------------------------------------
    /// Return whether this module should be considered for general searches.
    ///
    /// This API will be consulted by the SearchFilterForUnconstrainedSearches
    /// and any module that returns \b true will not be searched.  Note the
    /// SearchFilterForUnconstrainedSearches is the search filter that
    /// gets used in the CreateBreakpoint calls when no modules is provided.
    ///
    /// The target call at present just consults the Platform's call of the
    /// same name.
    ///
    /// FIXME: When we get time we should add a way for the user to set modules that they
    /// don't want searched, in addition to or instead of the platform ones.
    /// 
    /// @param[in] module_sp
    ///     A shared pointer reference to the module that checked.
    ///
    /// @return \b true if the module should be excluded, \b false otherwise.
    //------------------------------------------------------------------
    bool
    ModuleIsExcludedForUnconstrainedSearches (const lldb::ModuleSP &module_sp);

    const ArchSpec &
    GetArchitecture () const
    {
        return m_arch;
    }
    
    //------------------------------------------------------------------
    /// Set the architecture for this target.
    ///
    /// If the current target has no Images read in, then this just sets the architecture, which will
    /// be used to select the architecture of the ExecutableModule when that is set.
    /// If the current target has an ExecutableModule, then calling SetArchitecture with a different
    /// architecture from the currently selected one will reset the ExecutableModule to that slice
    /// of the file backing the ExecutableModule.  If the file backing the ExecutableModule does not
    /// contain a fork of this architecture, then this code will return false, and the architecture
    /// won't be changed.
    /// If the input arch_spec is the same as the already set architecture, this is a no-op.
    ///
    /// @param[in] arch_spec
    ///     The new architecture.
    ///
    /// @return
    ///     \b true if the architecture was successfully set, \bfalse otherwise.
    //------------------------------------------------------------------
    bool
    SetArchitecture (const ArchSpec &arch_spec);

    bool
    MergeArchitecture (const ArchSpec &arch_spec);

    Debugger &
    GetDebugger ()
    {
        return m_debugger;
    }

    size_t
    ReadMemoryFromFileCache (const Address& addr, 
                             void *dst, 
                             size_t dst_len, 
                             Error &error);

    // Reading memory through the target allows us to skip going to the process
    // for reading memory if possible and it allows us to try and read from 
    // any constant sections in our object files on disk. If you always want
    // live program memory, read straight from the process. If you possibly 
    // want to read from const sections in object files, read from the target.
    // This version of ReadMemory will try and read memory from the process
    // if the process is alive. The order is:
    // 1 - if (prefer_file_cache == true) then read from object file cache
    // 2 - if there is a valid process, try and read from its memory
    // 3 - if (prefer_file_cache == false) then read from object file cache
    size_t
    ReadMemory(const Address& addr,
               bool prefer_file_cache,
               void *dst,
               size_t dst_len,
               Error &error,
               lldb::addr_t *load_addr_ptr = nullptr);
    
    size_t
    ReadCStringFromMemory (const Address& addr, std::string &out_str, Error &error);
    
    size_t
    ReadCStringFromMemory (const Address& addr, char *dst, size_t dst_max_len, Error &result_error);
    
    size_t
    ReadScalarIntegerFromMemory (const Address& addr, 
                                 bool prefer_file_cache,
                                 uint32_t byte_size, 
                                 bool is_signed, 
                                 Scalar &scalar, 
                                 Error &error);

    uint64_t
    ReadUnsignedIntegerFromMemory (const Address& addr, 
                                   bool prefer_file_cache,
                                   size_t integer_byte_size, 
                                   uint64_t fail_value, 
                                   Error &error);

    bool
    ReadPointerFromMemory (const Address& addr, 
                           bool prefer_file_cache,
                           Error &error,
                           Address &pointer_addr);

    SectionLoadList&
    GetSectionLoadList()
    {
        return m_section_load_history.GetCurrentSectionLoadList();
    }

//    const SectionLoadList&
//    GetSectionLoadList() const
//    {
//        return const_cast<SectionLoadHistory *>(&m_section_load_history)->GetCurrentSectionLoadList();
//    }

    static Target *
    GetTargetFromContexts (const ExecutionContext *exe_ctx_ptr, 
                           const SymbolContext *sc_ptr);

    //------------------------------------------------------------------
    // lldb::ExecutionContextScope pure virtual functions
    //------------------------------------------------------------------
    lldb::TargetSP
    CalculateTarget() override;
    
    lldb::ProcessSP
    CalculateProcess() override;
    
    lldb::ThreadSP
    CalculateThread() override;
    
    lldb::StackFrameSP
    CalculateStackFrame() override;

    void
    CalculateExecutionContext(ExecutionContext &exe_ctx) override;

    PathMappingList &
    GetImageSearchPathList ();
    
    TypeSystem *
    GetScratchTypeSystemForLanguage (Error *error, lldb::LanguageType language, bool create_on_demand = true);
    
    PersistentExpressionState *
    GetPersistentExpressionStateForLanguage (lldb::LanguageType language);
    
    // Creates a UserExpression for the given language, the rest of the parameters have the
    // same meaning as for the UserExpression constructor.
    // Returns a new-ed object which the caller owns.
    
    UserExpression *
    GetUserExpressionForLanguage(const char *expr,
                                 const char *expr_prefix,
                                 lldb::LanguageType language,
                                 Expression::ResultType desired_type,
                                 const EvaluateExpressionOptions &options,
                                 Error &error);
    
    // Creates a FunctionCaller for the given language, the rest of the parameters have the
    // same meaning as for the FunctionCaller constructor.  Since a FunctionCaller can't be
    // IR Interpreted, it makes no sense to call this with an ExecutionContextScope that lacks
    // a Process.
    // Returns a new-ed object which the caller owns.
    
    FunctionCaller *
    GetFunctionCallerForLanguage (lldb::LanguageType language,
                                  const CompilerType &return_type,
                                  const Address& function_address,
                                  const ValueList &arg_value_list,
                                  const char *name,
                                  Error &error);
    
    // Creates a UtilityFunction for the given language, the rest of the parameters have the
    // same meaning as for the UtilityFunction constructor.
    // Returns a new-ed object which the caller owns.
    
    UtilityFunction *
    GetUtilityFunctionForLanguage (const char *expr,
                                   lldb::LanguageType language,
                                   const char *name,
                                   Error &error);

    ClangASTContext *
    GetScratchClangASTContext(bool create_on_demand=true);
    
    lldb::ClangASTImporterSP
    GetClangASTImporter();
    
    //----------------------------------------------------------------------
    // Install any files through the platform that need be to installed
    // prior to launching or attaching.
    //----------------------------------------------------------------------
    Error
    Install(ProcessLaunchInfo *launch_info);
    
    bool
    ResolveFileAddress (lldb::addr_t load_addr,
                        Address &so_addr);
    
    bool
    ResolveLoadAddress (lldb::addr_t load_addr,
                        Address &so_addr,
                        uint32_t stop_id = SectionLoadHistory::eStopIDNow);
    
    bool
    SetSectionLoadAddress (const lldb::SectionSP &section,
                           lldb::addr_t load_addr,
                           bool warn_multiple = false);

    size_t
    UnloadModuleSections (const lldb::ModuleSP &module_sp);

    size_t
    UnloadModuleSections (const ModuleList &module_list);

    bool
    SetSectionUnloaded (const lldb::SectionSP &section_sp);

    bool
    SetSectionUnloaded (const lldb::SectionSP &section_sp, lldb::addr_t load_addr);
    
    void
    ClearAllLoadedSections ();

    // Since expressions results can persist beyond the lifetime of a process,
    // and the const expression results are available after a process is gone,
    // we provide a way for expressions to be evaluated from the Target itself.
    // If an expression is going to be run, then it should have a frame filled
    // in in th execution context. 
    lldb::ExpressionResults
    EvaluateExpression (const char *expression,
                        ExecutionContextScope *exe_scope,
                        lldb::ValueObjectSP &result_valobj_sp,
                        const EvaluateExpressionOptions& options = EvaluateExpressionOptions());

    lldb::ExpressionVariableSP
    GetPersistentVariable(const ConstString &name);
    
    lldb::addr_t
    GetPersistentSymbol(const ConstString &name);
    
    //------------------------------------------------------------------
    // Target Stop Hooks
    //------------------------------------------------------------------
    class StopHook : public UserID
    {
    public:
        StopHook (const StopHook &rhs);

        ~StopHook ();

        StringList *
        GetCommandPointer ()
        {
            return &m_commands;
        }
        
        const StringList &
        GetCommands()
        {
            return m_commands;
        }
        
        lldb::TargetSP &
        GetTarget()
        {
            return m_target_sp;
        }
        
        void
        SetCommands (StringList &in_commands)
        {
            m_commands = in_commands;
        }
        
        // Set the specifier.  The stop hook will own the specifier, and is responsible for deleting it when we're done.
        void
        SetSpecifier (SymbolContextSpecifier *specifier);
        
        SymbolContextSpecifier *
        GetSpecifier ()
        {
            return m_specifier_sp.get();
        }
        
        // Set the Thread Specifier.  The stop hook will own the thread specifier, and is responsible for deleting it when we're done.
        void
        SetThreadSpecifier (ThreadSpec *specifier);
        
        ThreadSpec *
        GetThreadSpecifier()
        {
            return m_thread_spec_ap.get();
        }
        
        bool
        IsActive()
        {
            return m_active;
        }
        
        void
        SetIsActive (bool is_active)
        {
            m_active = is_active;
        }
        
        void
        GetDescription (Stream *s, lldb::DescriptionLevel level) const;
        
    private:
        lldb::TargetSP m_target_sp;
        StringList   m_commands;
        lldb::SymbolContextSpecifierSP m_specifier_sp;
        std::unique_ptr<ThreadSpec> m_thread_spec_ap;
        bool m_active;
        
        // Use CreateStopHook to make a new empty stop hook. The GetCommandPointer and fill it with commands,
        // and SetSpecifier to set the specifier shared pointer (can be null, that will match anything.)
        StopHook (lldb::TargetSP target_sp, lldb::user_id_t uid);
        friend class Target;
    };
    typedef std::shared_ptr<StopHook> StopHookSP;
    
    // Add an empty stop hook to the Target's stop hook list, and returns a shared pointer to it in new_hook.  
    // Returns the id of the new hook.        
    StopHookSP
    CreateStopHook ();
    
    void
    RunStopHooks ();
    
    size_t
    GetStopHookSize();
    
    bool
    SetSuppresStopHooks (bool suppress)
    {
        bool old_value = m_suppress_stop_hooks;
        m_suppress_stop_hooks = suppress;
        return old_value;
    }
    
    bool
    GetSuppressStopHooks ()
    {
        return m_suppress_stop_hooks;
    }

//    StopHookSP &
//    GetStopHookByIndex (size_t index);
//    
    bool
    RemoveStopHookByID (lldb::user_id_t uid);
    
    void
    RemoveAllStopHooks ();
    
    StopHookSP
    GetStopHookByID (lldb::user_id_t uid);
    
    bool
    SetStopHookActiveStateByID (lldb::user_id_t uid, bool active_state);
    
    void
    SetAllStopHooksActiveState (bool active_state);
    
    size_t GetNumStopHooks () const
    {
        return m_stop_hooks.size();
    }
    
    StopHookSP
    GetStopHookAtIndex (size_t index)
    {
        if (index >= GetNumStopHooks())
            return StopHookSP();
        StopHookCollection::iterator pos = m_stop_hooks.begin();
        
        while (index > 0)
        {
            pos++;
            index--;
        }
        return (*pos).second;
    }
    
    lldb::PlatformSP
    GetPlatform ()
    {
        return m_platform_sp;
    }

    void
    SetPlatform (const lldb::PlatformSP &platform_sp)
    {
        m_platform_sp = platform_sp;
    }

    SourceManager &
    GetSourceManager ();
    
    ClangModulesDeclVendor *
    GetClangModulesDeclVendor ();

    //------------------------------------------------------------------
    // Methods.
    //------------------------------------------------------------------
    lldb::SearchFilterSP
    GetSearchFilterForModule (const FileSpec *containingModule);

    lldb::SearchFilterSP
    GetSearchFilterForModuleList (const FileSpecList *containingModuleList);
    
    lldb::SearchFilterSP
    GetSearchFilterForModuleAndCUList (const FileSpecList *containingModules, const FileSpecList *containingSourceFiles);
    
    lldb::REPLSP
    GetREPL (Error &err, lldb::LanguageType language, const char *repl_options, bool can_create);
    
    void
    SetREPL (lldb::LanguageType language, lldb::REPLSP repl_sp);

protected:
    //------------------------------------------------------------------
    /// Implementing of ModuleList::Notifier.
    //------------------------------------------------------------------
    
    void
    ModuleAdded(const ModuleList& module_list, const lldb::ModuleSP& module_sp) override;
    
    void
    ModuleRemoved(const ModuleList& module_list, const lldb::ModuleSP& module_sp) override;
    
    void
    ModuleUpdated(const ModuleList& module_list,
		  const lldb::ModuleSP& old_module_sp,
		  const lldb::ModuleSP& new_module_sp) override;
    void
    WillClearList(const ModuleList& module_list) override;

    //------------------------------------------------------------------
    // Member variables.
    //------------------------------------------------------------------
    Debugger &      m_debugger;
    lldb::PlatformSP m_platform_sp;     ///< The platform for this target.
    Mutex           m_mutex;            ///< An API mutex that is used by the lldb::SB* classes make the SB interface thread safe
    ArchSpec        m_arch;
    ModuleList      m_images;           ///< The list of images for this process (shared libraries and anything dynamically loaded).
    SectionLoadHistory m_section_load_history;
    BreakpointList  m_breakpoint_list;
    BreakpointList  m_internal_breakpoint_list;
    lldb::BreakpointSP m_last_created_breakpoint;
    WatchpointList  m_watchpoint_list;
    lldb::WatchpointSP m_last_created_watchpoint;
    // We want to tightly control the process destruction process so
    // we can correctly tear down everything that we need to, so the only
    // class that knows about the process lifespan is this target class.
    lldb::ProcessSP m_process_sp;
    lldb::SearchFilterSP  m_search_filter_sp;
    PathMappingList m_image_search_paths;
    TypeSystemMap m_scratch_type_system_map;
    
    typedef std::map<lldb::LanguageType, lldb::REPLSP> REPLMap;
    REPLMap m_repl_map;
    
    lldb::ClangASTImporterSP m_ast_importer_sp;
    lldb::ClangModulesDeclVendorUP m_clang_modules_decl_vendor_ap;

    lldb::SourceManagerUP m_source_manager_ap;

    typedef std::map<lldb::user_id_t, StopHookSP> StopHookCollection;
    StopHookCollection      m_stop_hooks;
    lldb::user_id_t         m_stop_hook_next_id;
    bool                    m_valid;
    bool                    m_suppress_stop_hooks;
    bool                    m_is_dummy_target;
    
    static void
    ImageSearchPathsChanged (const PathMappingList &path_list,
                             void *baton);

private:
    //------------------------------------------------------------------
    /// Construct with optional file and arch.
    ///
    /// This member is private. Clients must use
    /// TargetList::CreateTarget(const FileSpec*, const ArchSpec*)
    /// so all targets can be tracked from the central target list.
    ///
    /// @see TargetList::CreateTarget(const FileSpec*, const ArchSpec*)
    //------------------------------------------------------------------
    Target (Debugger &debugger,
            const ArchSpec &target_arch,
            const lldb::PlatformSP &platform_sp,
            bool is_dummy_target);

    // Helper function.
    bool
    ProcessIsValid ();

    // Copy breakpoints, stop hooks and so forth from the dummy target:
    void
    PrimeFromDummyTarget(Target *dummy_target);

    void
    AddBreakpoint(lldb::BreakpointSP breakpoint_sp, bool internal);

    DISALLOW_COPY_AND_ASSIGN (Target);
};

} // namespace lldb_private

#endif // liblldb_Target_h_
