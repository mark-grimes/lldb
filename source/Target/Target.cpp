//===-- Target.cpp ----------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// C Includes
// C++ Includes
#include <mutex>
// Other libraries and framework includes
// Project includes
#include "lldb/Target/Target.h"
#include "lldb/Breakpoint/BreakpointResolver.h"
#include "lldb/Breakpoint/BreakpointResolverAddress.h"
#include "lldb/Breakpoint/BreakpointResolverFileLine.h"
#include "lldb/Breakpoint/BreakpointResolverFileRegex.h"
#include "lldb/Breakpoint/BreakpointResolverName.h"
#include "lldb/Breakpoint/Watchpoint.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/Event.h"
#include "lldb/Core/Log.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/ModuleSpec.h"
#include "lldb/Core/Section.h"
#include "lldb/Core/SourceManager.h"
#include "lldb/Core/State.h"
#include "lldb/Core/StreamFile.h"
#include "lldb/Core/StreamString.h"
#include "lldb/Core/Timer.h"
#include "lldb/Core/ValueObject.h"
#include "lldb/Expression/REPL.h"
#include "lldb/Expression/UserExpression.h"
#include "Plugins/ExpressionParser/Clang/ClangASTSource.h"
#include "Plugins/ExpressionParser/Clang/ClangPersistentVariables.h"
#include "Plugins/ExpressionParser/Clang/ClangModulesDeclVendor.h"
#include "lldb/Host/FileSpec.h"
#include "lldb/Host/Host.h"
#include "lldb/Interpreter/CommandInterpreter.h"
#include "lldb/Interpreter/CommandReturnObject.h"
#include "lldb/Interpreter/OptionGroupWatchpoint.h"
#include "lldb/Interpreter/OptionValues.h"
#include "lldb/Interpreter/Property.h"
#include "lldb/Symbol/ClangASTContext.h"
#include "lldb/Symbol/ObjectFile.h"
#include "lldb/Symbol/Function.h"
#include "lldb/Symbol/Symbol.h"
#include "lldb/Target/Language.h"
#include "lldb/Target/LanguageRuntime.h"
#include "lldb/Target/ObjCLanguageRuntime.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/SectionLoadList.h"
#include "lldb/Target/StackFrame.h"
#include "lldb/Target/SystemRuntime.h"
#include "lldb/Target/Thread.h"
#include "lldb/Target/ThreadSpec.h"
#include "lldb/Utility/LLDBAssert.h"

using namespace lldb;
using namespace lldb_private;

ConstString &
Target::GetStaticBroadcasterClass ()
{
    static ConstString class_name ("lldb.target");
    return class_name;
}

Target::Target(Debugger &debugger, const ArchSpec &target_arch, const lldb::PlatformSP &platform_sp, bool is_dummy_target) :
    TargetProperties (this),
    Broadcaster (debugger.GetBroadcasterManager(), Target::GetStaticBroadcasterClass().AsCString()),
    ExecutionContextScope (),
    m_debugger (debugger),
    m_platform_sp (platform_sp),
    m_mutex (Mutex::eMutexTypeRecursive), 
    m_arch (target_arch),
    m_images (this),
    m_section_load_history (),
    m_breakpoint_list (false),
    m_internal_breakpoint_list (true),
    m_watchpoint_list (),
    m_process_sp (),
    m_search_filter_sp (),
    m_image_search_paths (ImageSearchPathsChanged, this),
    m_ast_importer_sp (),
    m_source_manager_ap(),
    m_stop_hooks (),
    m_stop_hook_next_id (0),
    m_valid (true),
    m_suppress_stop_hooks (false),
    m_is_dummy_target(is_dummy_target)

{
    SetEventName (eBroadcastBitBreakpointChanged, "breakpoint-changed");
    SetEventName (eBroadcastBitModulesLoaded, "modules-loaded");
    SetEventName (eBroadcastBitModulesUnloaded, "modules-unloaded");
    SetEventName (eBroadcastBitWatchpointChanged, "watchpoint-changed");
    SetEventName (eBroadcastBitSymbolsLoaded, "symbols-loaded");

    CheckInWithManager();

    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_OBJECT));
    if (log)
        log->Printf ("%p Target::Target()", static_cast<void*>(this));
    if (m_arch.IsValid())
    {
        LogIfAnyCategoriesSet(LIBLLDB_LOG_TARGET, "Target::Target created with architecture %s (%s)", m_arch.GetArchitectureName(), m_arch.GetTriple().getTriple().c_str());
    }
}

Target::~Target()
{
    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_OBJECT));
    if (log)
        log->Printf ("%p Target::~Target()", static_cast<void*>(this));
    DeleteCurrentProcess ();
}

void
Target::PrimeFromDummyTarget(Target *target)
{
    if (!target)
        return;

    m_stop_hooks = target->m_stop_hooks;
    
    for (BreakpointSP breakpoint_sp : target->m_breakpoint_list.Breakpoints())
    {
        if (breakpoint_sp->IsInternal())
            continue;
            
        BreakpointSP new_bp (new Breakpoint (*this, *breakpoint_sp.get()));
        AddBreakpoint (new_bp, false);
    }
}

void
Target::Dump (Stream *s, lldb::DescriptionLevel description_level)
{
//    s->Printf("%.*p: ", (int)sizeof(void*) * 2, this);
    if (description_level != lldb::eDescriptionLevelBrief)
    {
        s->Indent();
        s->PutCString("Target\n");
        s->IndentMore();
            m_images.Dump(s);
            m_breakpoint_list.Dump(s);
            m_internal_breakpoint_list.Dump(s);
        s->IndentLess();
    }
    else
    {
        Module *exe_module = GetExecutableModulePointer();
        if (exe_module)
            s->PutCString (exe_module->GetFileSpec().GetFilename().GetCString());
        else
            s->PutCString ("No executable module.");
    }
}

void
Target::CleanupProcess ()
{
    // Do any cleanup of the target we need to do between process instances.
    // NB It is better to do this before destroying the process in case the
    // clean up needs some help from the process.
    m_breakpoint_list.ClearAllBreakpointSites();
    m_internal_breakpoint_list.ClearAllBreakpointSites();
    // Disable watchpoints just on the debugger side.
    Mutex::Locker locker;
    this->GetWatchpointList().GetListMutex(locker);
    DisableAllWatchpoints(false);
    ClearAllWatchpointHitCounts();
    ClearAllWatchpointHistoricValues();
}

void
Target::DeleteCurrentProcess ()
{
    if (m_process_sp)
    {
        m_section_load_history.Clear();
        if (m_process_sp->IsAlive())
            m_process_sp->Destroy(false);
        
        m_process_sp->Finalize();

        CleanupProcess ();

        m_process_sp.reset();
    }
}

const lldb::ProcessSP &
Target::CreateProcess (ListenerSP listener_sp, const char *plugin_name, const FileSpec *crash_file)
{
    DeleteCurrentProcess ();
    m_process_sp = Process::FindPlugin(shared_from_this(), plugin_name, listener_sp, crash_file);
    return m_process_sp;
}

const lldb::ProcessSP &
Target::GetProcessSP () const
{
    return m_process_sp;
}

lldb::REPLSP
Target::GetREPL (Error &err, lldb::LanguageType language, const char *repl_options, bool can_create)
{
    if (language == eLanguageTypeUnknown)
    {
        std::set<LanguageType> repl_languages;
        
        Language::GetLanguagesSupportingREPLs(repl_languages);
        
        if (repl_languages.size() == 1)
        {
            language = *repl_languages.begin();
        }
        else if (repl_languages.size() == 0)
        {
            err.SetErrorStringWithFormat("LLDB isn't configured with support support for any REPLs.");
            return REPLSP();
        }
        else
        {
            err.SetErrorStringWithFormat("Multiple possible REPL languages.  Please specify a language.");
            return REPLSP();
        }
    }
    
    REPLMap::iterator pos = m_repl_map.find(language);
    
    if (pos != m_repl_map.end())
    {
        return pos->second;
    }
    
    if (!can_create)
    {
        err.SetErrorStringWithFormat("Couldn't find an existing REPL for %s, and can't create a new one", Language::GetNameForLanguageType(language));
        return lldb::REPLSP();
    }
    
    Debugger *const debugger = nullptr;
    lldb::REPLSP ret = REPL::Create(err, language, debugger, this, repl_options);
    
    if (ret)
    {
        m_repl_map[language] = ret;
        return m_repl_map[language];
    }
    
    if (err.Success())
    {
        err.SetErrorStringWithFormat("Couldn't create a REPL for %s", Language::GetNameForLanguageType(language));
    }
    
    return lldb::REPLSP();
}

void
Target::SetREPL (lldb::LanguageType language, lldb::REPLSP repl_sp)
{
    lldbassert(!m_repl_map.count(language));
    
    m_repl_map[language] = repl_sp;
}

void
Target::Destroy()
{
    Mutex::Locker locker (m_mutex);
    m_valid = false;
    DeleteCurrentProcess ();
    m_platform_sp.reset();
    m_arch.Clear();
    ClearModules(true);
    m_section_load_history.Clear();
    const bool notify = false;
    m_breakpoint_list.RemoveAll(notify);
    m_internal_breakpoint_list.RemoveAll(notify);
    m_last_created_breakpoint.reset();
    m_last_created_watchpoint.reset();
    m_search_filter_sp.reset();
    m_image_search_paths.Clear(notify);
    m_stop_hooks.clear();
    m_stop_hook_next_id = 0;
    m_suppress_stop_hooks = false;
}

BreakpointList &
Target::GetBreakpointList(bool internal)
{
    if (internal)
        return m_internal_breakpoint_list;
    else
        return m_breakpoint_list;
}

const BreakpointList &
Target::GetBreakpointList(bool internal) const
{
    if (internal)
        return m_internal_breakpoint_list;
    else
        return m_breakpoint_list;
}

BreakpointSP
Target::GetBreakpointByID (break_id_t break_id)
{
    BreakpointSP bp_sp;

    if (LLDB_BREAK_ID_IS_INTERNAL (break_id))
        bp_sp = m_internal_breakpoint_list.FindBreakpointByID (break_id);
    else
        bp_sp = m_breakpoint_list.FindBreakpointByID (break_id);

    return bp_sp;
}

BreakpointSP
Target::CreateSourceRegexBreakpoint (const FileSpecList *containingModules,
                                     const FileSpecList *source_file_spec_list,
                                     RegularExpression &source_regex,
                                     bool internal,
                                     bool hardware,
                                     LazyBool move_to_nearest_code)
{
    SearchFilterSP filter_sp(GetSearchFilterForModuleAndCUList (containingModules, source_file_spec_list));
    if (move_to_nearest_code == eLazyBoolCalculate)
        move_to_nearest_code = GetMoveToNearestCode() ? eLazyBoolYes : eLazyBoolNo;
    BreakpointResolverSP resolver_sp(new BreakpointResolverFileRegex(nullptr, source_regex, !static_cast<bool>(move_to_nearest_code)));
    return CreateBreakpoint (filter_sp, resolver_sp, internal, hardware, true);
}

BreakpointSP
Target::CreateBreakpoint (const FileSpecList *containingModules,
                          const FileSpec &file,
                          uint32_t line_no,
                          LazyBool check_inlines,
                          LazyBool skip_prologue,
                          bool internal,
                          bool hardware,
                          LazyBool move_to_nearest_code)
{
    FileSpec remapped_file;
    ConstString remapped_path;
    if (GetSourcePathMap().ReverseRemapPath(ConstString(file.GetPath().c_str()), remapped_path))
        remapped_file.SetFile(remapped_path.AsCString(), true);
    else
        remapped_file = file;

    if (check_inlines == eLazyBoolCalculate)
    {
        const InlineStrategy inline_strategy = GetInlineStrategy();
        switch (inline_strategy)
        {
            case eInlineBreakpointsNever:
                check_inlines = eLazyBoolNo;
                break;
                
            case eInlineBreakpointsHeaders:
                if (remapped_file.IsSourceImplementationFile())
                    check_inlines = eLazyBoolNo;
                else
                    check_inlines = eLazyBoolYes;
                break;

            case eInlineBreakpointsAlways:
                check_inlines = eLazyBoolYes;
                break;
        }
    }
    SearchFilterSP filter_sp;
    if (check_inlines == eLazyBoolNo)
    {
        // Not checking for inlines, we are looking only for matching compile units
        FileSpecList compile_unit_list;
        compile_unit_list.Append (remapped_file);
        filter_sp = GetSearchFilterForModuleAndCUList (containingModules, &compile_unit_list);
    }
    else
    {
        filter_sp = GetSearchFilterForModuleList (containingModules);
    }
    if (skip_prologue == eLazyBoolCalculate)
        skip_prologue = GetSkipPrologue() ? eLazyBoolYes : eLazyBoolNo;
    if (move_to_nearest_code == eLazyBoolCalculate)
        move_to_nearest_code = GetMoveToNearestCode() ? eLazyBoolYes : eLazyBoolNo;

    BreakpointResolverSP resolver_sp(new BreakpointResolverFileLine(nullptr,
                                                                    remapped_file,
                                                                    line_no,
                                                                    check_inlines,
                                                                    skip_prologue,
                                                                    !static_cast<bool>(move_to_nearest_code)));
    return CreateBreakpoint (filter_sp, resolver_sp, internal, hardware, true);
}

BreakpointSP
Target::CreateBreakpoint (lldb::addr_t addr, bool internal, bool hardware)
{
    Address so_addr;
    
    // Check for any reason we want to move this breakpoint to other address.
    addr = GetBreakableLoadAddress(addr);

    // Attempt to resolve our load address if possible, though it is ok if
    // it doesn't resolve to section/offset.

    // Try and resolve as a load address if possible
    GetSectionLoadList().ResolveLoadAddress(addr, so_addr);
    if (!so_addr.IsValid())
    {
        // The address didn't resolve, so just set this as an absolute address
        so_addr.SetOffset (addr);
    }
    BreakpointSP bp_sp (CreateBreakpoint(so_addr, internal, hardware));
    return bp_sp;
}

BreakpointSP
Target::CreateBreakpoint (const Address &addr, bool internal, bool hardware)
{
    SearchFilterSP filter_sp(new SearchFilterForUnconstrainedSearches (shared_from_this()));
    BreakpointResolverSP resolver_sp(new BreakpointResolverAddress(nullptr, addr));
    return CreateBreakpoint (filter_sp, resolver_sp, internal, hardware, false);
}

lldb::BreakpointSP
Target::CreateAddressInModuleBreakpoint (lldb::addr_t file_addr,
                                         bool internal,
                                         const FileSpec *file_spec,
                                         bool request_hardware)
{
    SearchFilterSP filter_sp(new SearchFilterForUnconstrainedSearches (shared_from_this()));
    BreakpointResolverSP resolver_sp(new BreakpointResolverAddress(nullptr, file_addr, file_spec));
    return CreateBreakpoint (filter_sp, resolver_sp, internal, request_hardware, false);
}

BreakpointSP
Target::CreateBreakpoint (const FileSpecList *containingModules,
                          const FileSpecList *containingSourceFiles,
                          const char *func_name, 
                          uint32_t func_name_type_mask, 
                          LanguageType language,
                          LazyBool skip_prologue,
                          bool internal,
                          bool hardware)
{
    BreakpointSP bp_sp;
    if (func_name)
    {
        SearchFilterSP filter_sp(GetSearchFilterForModuleAndCUList (containingModules, containingSourceFiles));

        if (skip_prologue == eLazyBoolCalculate)
            skip_prologue = GetSkipPrologue() ? eLazyBoolYes : eLazyBoolNo;
        if (language == lldb::eLanguageTypeUnknown)
            language = GetLanguage();

        BreakpointResolverSP resolver_sp(new BreakpointResolverName(nullptr, 
                                                                    func_name, 
                                                                    func_name_type_mask, 
                                                                    language,
                                                                    Breakpoint::Exact, 
                                                                    skip_prologue));
        bp_sp = CreateBreakpoint (filter_sp, resolver_sp, internal, hardware, true);
    }
    return bp_sp;
}

lldb::BreakpointSP
Target::CreateBreakpoint (const FileSpecList *containingModules,
                          const FileSpecList *containingSourceFiles,
                          const std::vector<std::string> &func_names,
                          uint32_t func_name_type_mask,
                          LanguageType language,
                          LazyBool skip_prologue,
                          bool internal,
                          bool hardware)
{
    BreakpointSP bp_sp;
    size_t num_names = func_names.size();
    if (num_names > 0)
    {
        SearchFilterSP filter_sp(GetSearchFilterForModuleAndCUList (containingModules, containingSourceFiles));

        if (skip_prologue == eLazyBoolCalculate)
            skip_prologue = GetSkipPrologue() ? eLazyBoolYes : eLazyBoolNo;
        if (language == lldb::eLanguageTypeUnknown)
            language = GetLanguage();

        BreakpointResolverSP resolver_sp(new BreakpointResolverName(nullptr,
                                                                    func_names,
                                                                    func_name_type_mask,
                                                                    language,
                                                                    skip_prologue));
        bp_sp = CreateBreakpoint (filter_sp, resolver_sp, internal, hardware, true);
    }
    return bp_sp;
}

BreakpointSP
Target::CreateBreakpoint (const FileSpecList *containingModules,
                          const FileSpecList *containingSourceFiles,
                          const char *func_names[],
                          size_t num_names, 
                          uint32_t func_name_type_mask, 
                          LanguageType language,
                          LazyBool skip_prologue,
                          bool internal,
                          bool hardware)
{
    BreakpointSP bp_sp;
    if (num_names > 0)
    {
        SearchFilterSP filter_sp(GetSearchFilterForModuleAndCUList (containingModules, containingSourceFiles));
        
        if (skip_prologue == eLazyBoolCalculate)
            skip_prologue = GetSkipPrologue() ? eLazyBoolYes : eLazyBoolNo;
        if (language == lldb::eLanguageTypeUnknown)
            language = GetLanguage();

        BreakpointResolverSP resolver_sp(new BreakpointResolverName(nullptr,
                                                                    func_names,
                                                                    num_names, 
                                                                    func_name_type_mask,
                                                                    language,
                                                                    skip_prologue));
        bp_sp = CreateBreakpoint (filter_sp, resolver_sp, internal, hardware, true);
    }
    return bp_sp;
}

SearchFilterSP
Target::GetSearchFilterForModule (const FileSpec *containingModule)
{
    SearchFilterSP filter_sp;
    if (containingModule != nullptr)
    {
        // TODO: We should look into sharing module based search filters
        // across many breakpoints like we do for the simple target based one
        filter_sp.reset (new SearchFilterByModule (shared_from_this(), *containingModule));
    }
    else
    {
        if (!m_search_filter_sp)
            m_search_filter_sp.reset (new SearchFilterForUnconstrainedSearches (shared_from_this()));
        filter_sp = m_search_filter_sp;
    }
    return filter_sp;
}

SearchFilterSP
Target::GetSearchFilterForModuleList (const FileSpecList *containingModules)
{
    SearchFilterSP filter_sp;
    if (containingModules && containingModules->GetSize() != 0)
    {
        // TODO: We should look into sharing module based search filters
        // across many breakpoints like we do for the simple target based one
        filter_sp.reset (new SearchFilterByModuleList (shared_from_this(), *containingModules));
    }
    else
    {
        if (!m_search_filter_sp)
            m_search_filter_sp.reset (new SearchFilterForUnconstrainedSearches (shared_from_this()));
        filter_sp = m_search_filter_sp;
    }
    return filter_sp;
}

SearchFilterSP
Target::GetSearchFilterForModuleAndCUList (const FileSpecList *containingModules,
                                           const FileSpecList *containingSourceFiles)
{
    if (containingSourceFiles == nullptr || containingSourceFiles->GetSize() == 0)
        return GetSearchFilterForModuleList(containingModules);
        
    SearchFilterSP filter_sp;
    if (containingModules == nullptr)
    {
        // We could make a special "CU List only SearchFilter".  Better yet was if these could be composable, 
        // but that will take a little reworking.
        
        filter_sp.reset (new SearchFilterByModuleListAndCU (shared_from_this(), FileSpecList(), *containingSourceFiles));
    }
    else
    {
        filter_sp.reset (new SearchFilterByModuleListAndCU (shared_from_this(), *containingModules, *containingSourceFiles));
    }
    return filter_sp;
}

BreakpointSP
Target::CreateFuncRegexBreakpoint (const FileSpecList *containingModules, 
                                   const FileSpecList *containingSourceFiles,
                                   RegularExpression &func_regex, 
                                   lldb::LanguageType requested_language,
                                   LazyBool skip_prologue,
                                   bool internal,
                                   bool hardware)
{
    SearchFilterSP filter_sp(GetSearchFilterForModuleAndCUList (containingModules, containingSourceFiles));
    bool skip =
      (skip_prologue == eLazyBoolCalculate) ? GetSkipPrologue()
                                            : static_cast<bool>(skip_prologue);
    BreakpointResolverSP resolver_sp(new BreakpointResolverName(nullptr, 
                                                                func_regex,
                                                                requested_language,
                                                                skip));

    return CreateBreakpoint (filter_sp, resolver_sp, internal, hardware, true);
}

lldb::BreakpointSP
Target::CreateExceptionBreakpoint (enum lldb::LanguageType language, bool catch_bp, bool throw_bp, bool internal, Args *additional_args, Error *error)
{
    BreakpointSP exc_bkpt_sp = LanguageRuntime::CreateExceptionBreakpoint (*this, language, catch_bp, throw_bp, internal);
    if (exc_bkpt_sp && additional_args)
    {
        Breakpoint::BreakpointPreconditionSP precondition_sp = exc_bkpt_sp->GetPrecondition();
        if (precondition_sp && additional_args)
        {
            if (error)
                *error = precondition_sp->ConfigurePrecondition(*additional_args);
            else
                precondition_sp->ConfigurePrecondition(*additional_args);
        }
    }
    return exc_bkpt_sp;
}

BreakpointSP
Target::CreateBreakpoint (SearchFilterSP &filter_sp, BreakpointResolverSP &resolver_sp, bool internal, bool request_hardware, bool resolve_indirect_symbols)
{
    BreakpointSP bp_sp;
    if (filter_sp && resolver_sp)
    {
        bp_sp.reset(new Breakpoint (*this, filter_sp, resolver_sp, request_hardware, resolve_indirect_symbols));
        resolver_sp->SetBreakpoint (bp_sp.get());
        AddBreakpoint (bp_sp, internal);
    }
    return bp_sp;
}

void
Target::AddBreakpoint (lldb::BreakpointSP bp_sp, bool internal)
{
    if (!bp_sp)
        return;
    if (internal)
        m_internal_breakpoint_list.Add (bp_sp, false);
    else
        m_breakpoint_list.Add (bp_sp, true);

    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_BREAKPOINTS));
    if (log)
    {
        StreamString s;
        bp_sp->GetDescription(&s, lldb::eDescriptionLevelVerbose);
        log->Printf ("Target::%s (internal = %s) => break_id = %s\n", __FUNCTION__, bp_sp->IsInternal() ? "yes" : "no", s.GetData());
    }

    bp_sp->ResolveBreakpoint();

    if (!internal)
    {
        m_last_created_breakpoint = bp_sp;
    }
}

bool
Target::ProcessIsValid()
{
    return (m_process_sp && m_process_sp->IsAlive());
}

static bool
CheckIfWatchpointsExhausted(Target *target, Error &error)
{
    uint32_t num_supported_hardware_watchpoints;
    Error rc = target->GetProcessSP()->GetWatchpointSupportInfo(num_supported_hardware_watchpoints);
    if (rc.Success())
    {
        uint32_t num_current_watchpoints = target->GetWatchpointList().GetSize();
        if (num_current_watchpoints >= num_supported_hardware_watchpoints)
            error.SetErrorStringWithFormat("number of supported hardware watchpoints (%u) has been reached",
                                           num_supported_hardware_watchpoints);
    }
    return false;
}

// See also Watchpoint::SetWatchpointType(uint32_t type) and
// the OptionGroupWatchpoint::WatchType enum type.
WatchpointSP
Target::CreateWatchpoint(lldb::addr_t addr, size_t size, const CompilerType *type, uint32_t kind, Error &error)
{
    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_WATCHPOINTS));
    if (log)
        log->Printf("Target::%s (addr = 0x%8.8" PRIx64 " size = %" PRIu64 " type = %u)\n",
                    __FUNCTION__, addr, (uint64_t)size, kind);

    WatchpointSP wp_sp;
    if (!ProcessIsValid())
    {
        error.SetErrorString("process is not alive");
        return wp_sp;
    }
    
    if (addr == LLDB_INVALID_ADDRESS || size == 0)
    {
        if (size == 0)
            error.SetErrorString("cannot set a watchpoint with watch_size of 0");
        else
            error.SetErrorStringWithFormat("invalid watch address: %" PRIu64, addr);
        return wp_sp;
    }
    
    if (!LLDB_WATCH_TYPE_IS_VALID(kind))
    {
        error.SetErrorStringWithFormat ("invalid watchpoint type: %d", kind);
    }

    // Currently we only support one watchpoint per address, with total number
    // of watchpoints limited by the hardware which the inferior is running on.

    // Grab the list mutex while doing operations.
    const bool notify = false;   // Don't notify about all the state changes we do on creating the watchpoint.
    Mutex::Locker locker;
    this->GetWatchpointList().GetListMutex(locker);
    WatchpointSP matched_sp = m_watchpoint_list.FindByAddress(addr);
    if (matched_sp)
    {
        size_t old_size = matched_sp->GetByteSize();
        uint32_t old_type =
            (matched_sp->WatchpointRead() ? LLDB_WATCH_TYPE_READ : 0) |
            (matched_sp->WatchpointWrite() ? LLDB_WATCH_TYPE_WRITE : 0);
        // Return the existing watchpoint if both size and type match.
        if (size == old_size && kind == old_type)
        {
            wp_sp = matched_sp;
            wp_sp->SetEnabled(false, notify);
        }
        else
        {
            // Nil the matched watchpoint; we will be creating a new one.
            m_process_sp->DisableWatchpoint(matched_sp.get(), notify);
            m_watchpoint_list.Remove(matched_sp->GetID(), true);
        }
    }

    if (!wp_sp) 
    {
        wp_sp.reset(new Watchpoint(*this, addr, size, type));
        wp_sp->SetWatchpointType(kind, notify);
        m_watchpoint_list.Add (wp_sp, true);
    }

    error = m_process_sp->EnableWatchpoint(wp_sp.get(), notify);
    if (log)
        log->Printf("Target::%s (creation of watchpoint %s with id = %u)\n",
                    __FUNCTION__,
                    error.Success() ? "succeeded" : "failed",
                    wp_sp->GetID());

    if (error.Fail()) 
    {
        // Enabling the watchpoint on the device side failed.
        // Remove the said watchpoint from the list maintained by the target instance.
        m_watchpoint_list.Remove (wp_sp->GetID(), true);
        // See if we could provide more helpful error message.
        if (!CheckIfWatchpointsExhausted(this, error))
        {
            if (!OptionGroupWatchpoint::IsWatchSizeSupported(size))
                error.SetErrorStringWithFormat("watch size of %" PRIu64 " is not supported", (uint64_t)size);
        }
        wp_sp.reset();
    }
    else
        m_last_created_watchpoint = wp_sp;
    return wp_sp;
}

void
Target::RemoveAllBreakpoints (bool internal_also)
{
    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_BREAKPOINTS));
    if (log)
        log->Printf ("Target::%s (internal_also = %s)\n", __FUNCTION__, internal_also ? "yes" : "no");

    m_breakpoint_list.RemoveAll (true);
    if (internal_also)
        m_internal_breakpoint_list.RemoveAll (false);
        
    m_last_created_breakpoint.reset();
}

void
Target::DisableAllBreakpoints (bool internal_also)
{
    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_BREAKPOINTS));
    if (log)
        log->Printf ("Target::%s (internal_also = %s)\n", __FUNCTION__, internal_also ? "yes" : "no");

    m_breakpoint_list.SetEnabledAll (false);
    if (internal_also)
        m_internal_breakpoint_list.SetEnabledAll (false);
}

void
Target::EnableAllBreakpoints (bool internal_also)
{
    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_BREAKPOINTS));
    if (log)
        log->Printf ("Target::%s (internal_also = %s)\n", __FUNCTION__, internal_also ? "yes" : "no");

    m_breakpoint_list.SetEnabledAll (true);
    if (internal_also)
        m_internal_breakpoint_list.SetEnabledAll (true);
}

bool
Target::RemoveBreakpointByID (break_id_t break_id)
{
    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_BREAKPOINTS));
    if (log)
        log->Printf ("Target::%s (break_id = %i, internal = %s)\n", __FUNCTION__, break_id, LLDB_BREAK_ID_IS_INTERNAL (break_id) ? "yes" : "no");

    if (DisableBreakpointByID (break_id))
    {
        if (LLDB_BREAK_ID_IS_INTERNAL (break_id))
            m_internal_breakpoint_list.Remove(break_id, false);
        else
        {
            if (m_last_created_breakpoint)
            {
                if (m_last_created_breakpoint->GetID() == break_id)
                    m_last_created_breakpoint.reset();
            }
            m_breakpoint_list.Remove(break_id, true);
        }
        return true;
    }
    return false;
}

bool
Target::DisableBreakpointByID (break_id_t break_id)
{
    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_BREAKPOINTS));
    if (log)
        log->Printf ("Target::%s (break_id = %i, internal = %s)\n", __FUNCTION__, break_id, LLDB_BREAK_ID_IS_INTERNAL (break_id) ? "yes" : "no");

    BreakpointSP bp_sp;

    if (LLDB_BREAK_ID_IS_INTERNAL (break_id))
        bp_sp = m_internal_breakpoint_list.FindBreakpointByID (break_id);
    else
        bp_sp = m_breakpoint_list.FindBreakpointByID (break_id);
    if (bp_sp)
    {
        bp_sp->SetEnabled (false);
        return true;
    }
    return false;
}

bool
Target::EnableBreakpointByID (break_id_t break_id)
{
    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_BREAKPOINTS));
    if (log)
        log->Printf ("Target::%s (break_id = %i, internal = %s)\n",
                     __FUNCTION__,
                     break_id,
                     LLDB_BREAK_ID_IS_INTERNAL (break_id) ? "yes" : "no");

    BreakpointSP bp_sp;

    if (LLDB_BREAK_ID_IS_INTERNAL (break_id))
        bp_sp = m_internal_breakpoint_list.FindBreakpointByID (break_id);
    else
        bp_sp = m_breakpoint_list.FindBreakpointByID (break_id);

    if (bp_sp)
    {
        bp_sp->SetEnabled (true);
        return true;
    }
    return false;
}

// The flag 'end_to_end', default to true, signifies that the operation is
// performed end to end, for both the debugger and the debuggee.

// Assumption: Caller holds the list mutex lock for m_watchpoint_list for end
// to end operations.
bool
Target::RemoveAllWatchpoints (bool end_to_end)
{
    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_WATCHPOINTS));
    if (log)
        log->Printf ("Target::%s\n", __FUNCTION__);

    if (!end_to_end) {
        m_watchpoint_list.RemoveAll(true);
        return true;
    }

    // Otherwise, it's an end to end operation.

    if (!ProcessIsValid())
        return false;

    size_t num_watchpoints = m_watchpoint_list.GetSize();
    for (size_t i = 0; i < num_watchpoints; ++i)
    {
        WatchpointSP wp_sp = m_watchpoint_list.GetByIndex(i);
        if (!wp_sp)
            return false;

        Error rc = m_process_sp->DisableWatchpoint(wp_sp.get());
        if (rc.Fail())
            return false;
    }
    m_watchpoint_list.RemoveAll (true);
    m_last_created_watchpoint.reset();
    return true; // Success!
}

// Assumption: Caller holds the list mutex lock for m_watchpoint_list for end to
// end operations.
bool
Target::DisableAllWatchpoints (bool end_to_end)
{
    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_WATCHPOINTS));
    if (log)
        log->Printf ("Target::%s\n", __FUNCTION__);

    if (!end_to_end) {
        m_watchpoint_list.SetEnabledAll(false);
        return true;
    }

    // Otherwise, it's an end to end operation.

    if (!ProcessIsValid())
        return false;

    size_t num_watchpoints = m_watchpoint_list.GetSize();
    for (size_t i = 0; i < num_watchpoints; ++i)
    {
        WatchpointSP wp_sp = m_watchpoint_list.GetByIndex(i);
        if (!wp_sp)
            return false;

        Error rc = m_process_sp->DisableWatchpoint(wp_sp.get());
        if (rc.Fail())
            return false;
    }
    return true; // Success!
}

// Assumption: Caller holds the list mutex lock for m_watchpoint_list for end to
// end operations.
bool
Target::EnableAllWatchpoints (bool end_to_end)
{
    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_WATCHPOINTS));
    if (log)
        log->Printf ("Target::%s\n", __FUNCTION__);

    if (!end_to_end) {
        m_watchpoint_list.SetEnabledAll(true);
        return true;
    }

    // Otherwise, it's an end to end operation.

    if (!ProcessIsValid())
        return false;

    size_t num_watchpoints = m_watchpoint_list.GetSize();
    for (size_t i = 0; i < num_watchpoints; ++i)
    {
        WatchpointSP wp_sp = m_watchpoint_list.GetByIndex(i);
        if (!wp_sp)
            return false;

        Error rc = m_process_sp->EnableWatchpoint(wp_sp.get());
        if (rc.Fail())
            return false;
    }
    return true; // Success!
}

// Assumption: Caller holds the list mutex lock for m_watchpoint_list.
bool
Target::ClearAllWatchpointHitCounts ()
{
    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_WATCHPOINTS));
    if (log)
        log->Printf ("Target::%s\n", __FUNCTION__);

    size_t num_watchpoints = m_watchpoint_list.GetSize();
    for (size_t i = 0; i < num_watchpoints; ++i)
    {
        WatchpointSP wp_sp = m_watchpoint_list.GetByIndex(i);
        if (!wp_sp)
            return false;

        wp_sp->ResetHitCount();
    }
    return true; // Success!
}

// Assumption: Caller holds the list mutex lock for m_watchpoint_list.
bool
Target::ClearAllWatchpointHistoricValues ()
{
    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_WATCHPOINTS));
    if (log)
        log->Printf ("Target::%s\n", __FUNCTION__);
    
    size_t num_watchpoints = m_watchpoint_list.GetSize();
    for (size_t i = 0; i < num_watchpoints; ++i)
    {
        WatchpointSP wp_sp = m_watchpoint_list.GetByIndex(i);
        if (!wp_sp)
            return false;
        
        wp_sp->ResetHistoricValues();
    }
    return true; // Success!
}

// Assumption: Caller holds the list mutex lock for m_watchpoint_list
// during these operations.
bool
Target::IgnoreAllWatchpoints (uint32_t ignore_count)
{
    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_WATCHPOINTS));
    if (log)
        log->Printf ("Target::%s\n", __FUNCTION__);

    if (!ProcessIsValid())
        return false;

    size_t num_watchpoints = m_watchpoint_list.GetSize();
    for (size_t i = 0; i < num_watchpoints; ++i)
    {
        WatchpointSP wp_sp = m_watchpoint_list.GetByIndex(i);
        if (!wp_sp)
            return false;

        wp_sp->SetIgnoreCount(ignore_count);
    }
    return true; // Success!
}

// Assumption: Caller holds the list mutex lock for m_watchpoint_list.
bool
Target::DisableWatchpointByID (lldb::watch_id_t watch_id)
{
    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_WATCHPOINTS));
    if (log)
        log->Printf ("Target::%s (watch_id = %i)\n", __FUNCTION__, watch_id);

    if (!ProcessIsValid())
        return false;

    WatchpointSP wp_sp = m_watchpoint_list.FindByID (watch_id);
    if (wp_sp)
    {
        Error rc = m_process_sp->DisableWatchpoint(wp_sp.get());
        if (rc.Success())
            return true;

        // Else, fallthrough.
    }
    return false;
}

// Assumption: Caller holds the list mutex lock for m_watchpoint_list.
bool
Target::EnableWatchpointByID (lldb::watch_id_t watch_id)
{
    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_WATCHPOINTS));
    if (log)
        log->Printf ("Target::%s (watch_id = %i)\n", __FUNCTION__, watch_id);

    if (!ProcessIsValid())
        return false;

    WatchpointSP wp_sp = m_watchpoint_list.FindByID (watch_id);
    if (wp_sp)
    {
        Error rc = m_process_sp->EnableWatchpoint(wp_sp.get());
        if (rc.Success())
            return true;

        // Else, fallthrough.
    }
    return false;
}

// Assumption: Caller holds the list mutex lock for m_watchpoint_list.
bool
Target::RemoveWatchpointByID (lldb::watch_id_t watch_id)
{
    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_WATCHPOINTS));
    if (log)
        log->Printf ("Target::%s (watch_id = %i)\n", __FUNCTION__, watch_id);

    WatchpointSP watch_to_remove_sp = m_watchpoint_list.FindByID(watch_id);
    if (watch_to_remove_sp == m_last_created_watchpoint)
        m_last_created_watchpoint.reset();
        
    if (DisableWatchpointByID (watch_id))
    {
        m_watchpoint_list.Remove(watch_id, true);
        return true;
    }
    return false;
}

// Assumption: Caller holds the list mutex lock for m_watchpoint_list.
bool
Target::IgnoreWatchpointByID (lldb::watch_id_t watch_id, uint32_t ignore_count)
{
    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_WATCHPOINTS));
    if (log)
        log->Printf ("Target::%s (watch_id = %i)\n", __FUNCTION__, watch_id);

    if (!ProcessIsValid())
        return false;

    WatchpointSP wp_sp = m_watchpoint_list.FindByID (watch_id);
    if (wp_sp)
    {
        wp_sp->SetIgnoreCount(ignore_count);
        return true;
    }
    return false;
}

ModuleSP
Target::GetExecutableModule ()
{
    // search for the first executable in the module list
    for (size_t i = 0; i < m_images.GetSize(); ++i)
    {
        ModuleSP module_sp = m_images.GetModuleAtIndex (i);
        lldb_private::ObjectFile * obj = module_sp->GetObjectFile();
        if (obj == nullptr)
            continue;
        if (obj->GetType() == ObjectFile::Type::eTypeExecutable)
            return module_sp;
    }
    // as fall back return the first module loaded
    return m_images.GetModuleAtIndex (0);
}

Module*
Target::GetExecutableModulePointer ()
{
    return GetExecutableModule().get();
}

static void
LoadScriptingResourceForModule (const ModuleSP &module_sp, Target *target)
{
    Error error;
    StreamString feedback_stream;
    if (module_sp && !module_sp->LoadScriptingResourceInTarget(target, error, &feedback_stream))
    {
        if (error.AsCString())
            target->GetDebugger().GetErrorFile()->Printf("unable to load scripting data for module %s - error reported was %s\n",
                                                           module_sp->GetFileSpec().GetFileNameStrippingExtension().GetCString(),
                                                           error.AsCString());
    }
    if (feedback_stream.GetSize())
        target->GetDebugger().GetErrorFile()->Printf("%s\n",
                                                     feedback_stream.GetData());
}

void
Target::ClearModules(bool delete_locations)
{
    ModulesDidUnload (m_images, delete_locations);
    m_section_load_history.Clear();
    m_images.Clear();
    m_scratch_type_system_map.Clear();
    m_ast_importer_sp.reset();
}

void
Target::DidExec ()
{
    // When a process exec's we need to know about it so we can do some cleanup. 
    m_breakpoint_list.RemoveInvalidLocations(m_arch);
    m_internal_breakpoint_list.RemoveInvalidLocations(m_arch);
}

void
Target::SetExecutableModule (ModuleSP& executable_sp, bool get_dependent_files)
{
    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_TARGET));
    ClearModules(false);
    
    if (executable_sp)
    {
        Timer scoped_timer (__PRETTY_FUNCTION__,
                            "Target::SetExecutableModule (executable = '%s')",
                            executable_sp->GetFileSpec().GetPath().c_str());

        m_images.Append(executable_sp); // The first image is our executable file

        // If we haven't set an architecture yet, reset our architecture based on what we found in the executable module.
        if (!m_arch.IsValid())
        {
            m_arch = executable_sp->GetArchitecture();
            if (log)
              log->Printf ("Target::SetExecutableModule setting architecture to %s (%s) based on executable file", m_arch.GetArchitectureName(), m_arch.GetTriple().getTriple().c_str());
        }

        FileSpecList dependent_files;
        ObjectFile *executable_objfile = executable_sp->GetObjectFile();

        if (executable_objfile && get_dependent_files)
        {
            executable_objfile->GetDependentModules(dependent_files);
            for (uint32_t i=0; i<dependent_files.GetSize(); i++)
            {
                FileSpec dependent_file_spec (dependent_files.GetFileSpecPointerAtIndex(i));
                FileSpec platform_dependent_file_spec;
                if (m_platform_sp)
                    m_platform_sp->GetFileWithUUID(dependent_file_spec, nullptr, platform_dependent_file_spec);
                else
                    platform_dependent_file_spec = dependent_file_spec;

                ModuleSpec module_spec (platform_dependent_file_spec, m_arch);
                ModuleSP image_module_sp(GetSharedModule (module_spec));
                if (image_module_sp)
                {
                    ObjectFile *objfile = image_module_sp->GetObjectFile();
                    if (objfile)
                        objfile->GetDependentModules(dependent_files);
                }
            }
        }
    }
}

bool
Target::SetArchitecture (const ArchSpec &arch_spec)
{
    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_TARGET));
    bool missing_local_arch = !m_arch.IsValid();
    bool replace_local_arch = true;
    bool compatible_local_arch = false;
    ArchSpec other(arch_spec);

    if (!missing_local_arch)
    {
        if (m_arch.IsCompatibleMatch(arch_spec))
        {
            other.MergeFrom(m_arch);
            
            if (m_arch.IsCompatibleMatch(other))
            {
                compatible_local_arch = true;
                bool arch_changed, vendor_changed, os_changed, os_ver_changed, env_changed;
                
                m_arch.PiecewiseTripleCompare(other,
                                              arch_changed,
                                              vendor_changed,
                                              os_changed,
                                              os_ver_changed,
                                              env_changed);
                
                if (!arch_changed && !vendor_changed && !os_changed)
                    replace_local_arch = false;
            }
        }
    }

    if (compatible_local_arch || missing_local_arch)
    {
        // If we haven't got a valid arch spec, or the architectures are compatible
        // update the architecture, unless the one we already have is more specified
        if (replace_local_arch)
            m_arch = other;
        if (log)
            log->Printf ("Target::SetArchitecture set architecture to %s (%s)", m_arch.GetArchitectureName(), m_arch.GetTriple().getTriple().c_str());
        return true;
    }
    
    // If we have an executable file, try to reset the executable to the desired architecture
    if (log)
      log->Printf ("Target::SetArchitecture changing architecture to %s (%s)", arch_spec.GetArchitectureName(), arch_spec.GetTriple().getTriple().c_str());
    m_arch = other;
    ModuleSP executable_sp = GetExecutableModule ();

    ClearModules(true);
    // Need to do something about unsetting breakpoints.
    
    if (executable_sp)
    {
        if (log)
          log->Printf("Target::SetArchitecture Trying to select executable file architecture %s (%s)", arch_spec.GetArchitectureName(), arch_spec.GetTriple().getTriple().c_str());
        ModuleSpec module_spec (executable_sp->GetFileSpec(), other);
        Error error = ModuleList::GetSharedModule(module_spec, 
                                                  executable_sp, 
                                                  &GetExecutableSearchPaths(),
                                                  nullptr, 
                                                  nullptr);
                                      
        if (!error.Fail() && executable_sp)
        {
            SetExecutableModule (executable_sp, true);
            return true;
        }
    }
    return false;
}

bool
Target::MergeArchitecture (const ArchSpec &arch_spec)
{
    if (arch_spec.IsValid())
    {
        if (m_arch.IsCompatibleMatch(arch_spec))
        {
            // The current target arch is compatible with "arch_spec", see if we
            // can improve our current architecture using bits from "arch_spec"

            // Merge bits from arch_spec into "merged_arch" and set our architecture
            ArchSpec merged_arch (m_arch);
            merged_arch.MergeFrom (arch_spec);
            return SetArchitecture(merged_arch);
        }
        else
        {
            // The new architecture is different, we just need to replace it
            return SetArchitecture(arch_spec);
        }
    }
    return false;
}

void
Target::WillClearList (const ModuleList& module_list)
{
}

void
Target::ModuleAdded (const ModuleList& module_list, const ModuleSP &module_sp)
{
    // A module is being added to this target for the first time
    if (m_valid)
    {
        ModuleList my_module_list;
        my_module_list.Append(module_sp);
        LoadScriptingResourceForModule(module_sp, this);
        ModulesDidLoad (my_module_list);
    }
}

void
Target::ModuleRemoved (const ModuleList& module_list, const ModuleSP &module_sp)
{
    // A module is being removed from this target.
    if (m_valid)
    {
        ModuleList my_module_list;
        my_module_list.Append(module_sp);
        ModulesDidUnload (my_module_list, false);
    }
}

void
Target::ModuleUpdated (const ModuleList& module_list, const ModuleSP &old_module_sp, const ModuleSP &new_module_sp)
{
    // A module is replacing an already added module
    if (m_valid)
    {
        m_breakpoint_list.UpdateBreakpointsWhenModuleIsReplaced(old_module_sp, new_module_sp);
        m_internal_breakpoint_list.UpdateBreakpointsWhenModuleIsReplaced(old_module_sp, new_module_sp);
    }
}

void
Target::ModulesDidLoad (ModuleList &module_list)
{
    if (m_valid && module_list.GetSize())
    {
        m_breakpoint_list.UpdateBreakpoints (module_list, true, false);
        m_internal_breakpoint_list.UpdateBreakpoints (module_list, true, false);
        if (m_process_sp)
        {
            m_process_sp->ModulesDidLoad (module_list);
        }
        BroadcastEvent (eBroadcastBitModulesLoaded, new TargetEventData (this->shared_from_this(), module_list));
    }
}

void
Target::SymbolsDidLoad (ModuleList &module_list)
{
    if (m_valid && module_list.GetSize())
    {
        if (m_process_sp)
        {
            LanguageRuntime* runtime = m_process_sp->GetLanguageRuntime(lldb::eLanguageTypeObjC);
            if (runtime)
            {
                ObjCLanguageRuntime *objc_runtime = (ObjCLanguageRuntime*)runtime;
                objc_runtime->SymbolsDidLoad(module_list);
            }
        }
        
        m_breakpoint_list.UpdateBreakpoints (module_list, true, false);
        m_internal_breakpoint_list.UpdateBreakpoints (module_list, true, false);
        BroadcastEvent (eBroadcastBitSymbolsLoaded, new TargetEventData (this->shared_from_this(), module_list));
    }
}

void
Target::ModulesDidUnload (ModuleList &module_list, bool delete_locations)
{
    if (m_valid && module_list.GetSize())
    {
        UnloadModuleSections (module_list);
        m_breakpoint_list.UpdateBreakpoints (module_list, false, delete_locations);
        m_internal_breakpoint_list.UpdateBreakpoints (module_list, false, delete_locations);
        BroadcastEvent (eBroadcastBitModulesUnloaded, new TargetEventData (this->shared_from_this(), module_list));
    }
}

bool
Target::ModuleIsExcludedForUnconstrainedSearches (const FileSpec &module_file_spec)
{
    if (GetBreakpointsConsultPlatformAvoidList())
    {
        ModuleList matchingModules;
        ModuleSpec module_spec (module_file_spec);
        size_t num_modules = GetImages().FindModules(module_spec, matchingModules);
        
        // If there is more than one module for this file spec, only return true if ALL the modules are on the
        // black list.
        if (num_modules > 0)
        {
            for (size_t i  = 0; i < num_modules; i++)
            {
                if (!ModuleIsExcludedForUnconstrainedSearches (matchingModules.GetModuleAtIndex(i)))
                    return false;
            }
            return true;
        }
    }
    return false;
}

bool
Target::ModuleIsExcludedForUnconstrainedSearches (const lldb::ModuleSP &module_sp)
{
    if (GetBreakpointsConsultPlatformAvoidList())
    {
        if (m_platform_sp)
            return m_platform_sp->ModuleIsExcludedForUnconstrainedSearches (*this, module_sp);
    }
    return false;
}

size_t
Target::ReadMemoryFromFileCache (const Address& addr, void *dst, size_t dst_len, Error &error)
{
    SectionSP section_sp (addr.GetSection());
    if (section_sp)
    {
        // If the contents of this section are encrypted, the on-disk file is unusable.  Read only from live memory.
        if (section_sp->IsEncrypted())
        {
            error.SetErrorString("section is encrypted");
            return 0;
        }
        ModuleSP module_sp (section_sp->GetModule());
        if (module_sp)
        {
            ObjectFile *objfile = section_sp->GetModule()->GetObjectFile();
            if (objfile)
            {
                size_t bytes_read = objfile->ReadSectionData (section_sp.get(), 
                                                              addr.GetOffset(), 
                                                              dst, 
                                                              dst_len);
                if (bytes_read > 0)
                    return bytes_read;
                else
                    error.SetErrorStringWithFormat("error reading data from section %s", section_sp->GetName().GetCString());
            }
            else
                error.SetErrorString("address isn't from a object file");
        }
        else
            error.SetErrorString("address isn't in a module");
    }
    else
        error.SetErrorString("address doesn't contain a section that points to a section in a object file");

    return 0;
}

size_t
Target::ReadMemory (const Address& addr,
                    bool prefer_file_cache,
                    void *dst,
                    size_t dst_len,
                    Error &error,
                    lldb::addr_t *load_addr_ptr)
{
    error.Clear();
    
    // if we end up reading this from process memory, we will fill this
    // with the actual load address
    if (load_addr_ptr)
        *load_addr_ptr = LLDB_INVALID_ADDRESS;
    
    size_t bytes_read = 0;

    addr_t load_addr = LLDB_INVALID_ADDRESS;
    addr_t file_addr = LLDB_INVALID_ADDRESS;
    Address resolved_addr;
    if (!addr.IsSectionOffset())
    {
        SectionLoadList &section_load_list = GetSectionLoadList();
        if (section_load_list.IsEmpty())
        {
            // No sections are loaded, so we must assume we are not running
            // yet and anything we are given is a file address.
            file_addr = addr.GetOffset(); // "addr" doesn't have a section, so its offset is the file address
            m_images.ResolveFileAddress (file_addr, resolved_addr);            
        }
        else
        {
            // We have at least one section loaded. This can be because
            // we have manually loaded some sections with "target modules load ..."
            // or because we have have a live process that has sections loaded
            // through the dynamic loader
            load_addr = addr.GetOffset(); // "addr" doesn't have a section, so its offset is the load address
            section_load_list.ResolveLoadAddress (load_addr, resolved_addr);
        }
    }
    if (!resolved_addr.IsValid())
        resolved_addr = addr;

    if (prefer_file_cache)
    {
        bytes_read = ReadMemoryFromFileCache (resolved_addr, dst, dst_len, error);
        if (bytes_read > 0)
            return bytes_read;
    }
    
    if (ProcessIsValid())
    {
        if (load_addr == LLDB_INVALID_ADDRESS)
            load_addr = resolved_addr.GetLoadAddress (this);

        if (load_addr == LLDB_INVALID_ADDRESS)
        {
            ModuleSP addr_module_sp (resolved_addr.GetModule());
            if (addr_module_sp && addr_module_sp->GetFileSpec())
                error.SetErrorStringWithFormat("%s[0x%" PRIx64 "] can't be resolved, %s in not currently loaded",
                                               addr_module_sp->GetFileSpec().GetFilename().AsCString("<Unknown>"),
                                               resolved_addr.GetFileAddress(),
                                               addr_module_sp->GetFileSpec().GetFilename().AsCString("<Unknonw>"));
            else
                error.SetErrorStringWithFormat("0x%" PRIx64 " can't be resolved", resolved_addr.GetFileAddress());
        }
        else
        {
            bytes_read = m_process_sp->ReadMemory(load_addr, dst, dst_len, error);
            if (bytes_read != dst_len)
            {
                if (error.Success())
                {
                    if (bytes_read == 0)
                        error.SetErrorStringWithFormat("read memory from 0x%" PRIx64 " failed", load_addr);
                    else
                        error.SetErrorStringWithFormat("only %" PRIu64 " of %" PRIu64 " bytes were read from memory at 0x%" PRIx64, (uint64_t)bytes_read, (uint64_t)dst_len, load_addr);
                }
            }
            if (bytes_read)
            {
                if (load_addr_ptr)
                    *load_addr_ptr = load_addr;
                return bytes_read;
            }
            // If the address is not section offset we have an address that
            // doesn't resolve to any address in any currently loaded shared
            // libraries and we failed to read memory so there isn't anything
            // more we can do. If it is section offset, we might be able to
            // read cached memory from the object file.
            if (!resolved_addr.IsSectionOffset())
                return 0;
        }
    }
    
    if (!prefer_file_cache && resolved_addr.IsSectionOffset())
    {
        // If we didn't already try and read from the object file cache, then
        // try it after failing to read from the process.
        return ReadMemoryFromFileCache (resolved_addr, dst, dst_len, error);
    }
    return 0;
}

size_t
Target::ReadCStringFromMemory (const Address& addr, std::string &out_str, Error &error)
{
    char buf[256];
    out_str.clear();
    addr_t curr_addr = addr.GetLoadAddress(this);
    Address address(addr);
    while (1)
    {
        size_t length = ReadCStringFromMemory (address, buf, sizeof(buf), error);
        if (length == 0)
            break;
        out_str.append(buf, length);
        // If we got "length - 1" bytes, we didn't get the whole C string, we
        // need to read some more characters
        if (length == sizeof(buf) - 1)
            curr_addr += length;
        else
            break;
        address = Address(curr_addr);
    }
    return out_str.size();
}

size_t
Target::ReadCStringFromMemory (const Address& addr, char *dst, size_t dst_max_len, Error &result_error)
{
    size_t total_cstr_len = 0;
    if (dst && dst_max_len)
    {
        result_error.Clear();
        // NULL out everything just to be safe
        memset (dst, 0, dst_max_len);
        Error error;
        addr_t curr_addr = addr.GetLoadAddress(this);
        Address address(addr);

        // We could call m_process_sp->GetMemoryCacheLineSize() but I don't
        // think this really needs to be tied to the memory cache subsystem's
        // cache line size, so leave this as a fixed constant.
        const size_t cache_line_size = 512;

        size_t bytes_left = dst_max_len - 1;
        char *curr_dst = dst;
        
        while (bytes_left > 0)
        {
            addr_t cache_line_bytes_left = cache_line_size - (curr_addr % cache_line_size);
            addr_t bytes_to_read = std::min<addr_t>(bytes_left, cache_line_bytes_left);
            size_t bytes_read = ReadMemory (address, false, curr_dst, bytes_to_read, error);
            
            if (bytes_read == 0)
            {
                result_error = error;
                dst[total_cstr_len] = '\0';
                break;
            }
            const size_t len = strlen(curr_dst);
            
            total_cstr_len += len;
            
            if (len < bytes_to_read)
                break;
            
            curr_dst += bytes_read;
            curr_addr += bytes_read;
            bytes_left -= bytes_read;
            address = Address(curr_addr);
        }
    }
    else
    {
        if (dst == nullptr)
            result_error.SetErrorString("invalid arguments");
        else
            result_error.Clear();
    }
    return total_cstr_len;
}

size_t
Target::ReadScalarIntegerFromMemory (const Address& addr, 
                                     bool prefer_file_cache,
                                     uint32_t byte_size, 
                                     bool is_signed, 
                                     Scalar &scalar, 
                                     Error &error)
{
    uint64_t uval;
    
    if (byte_size <= sizeof(uval))
    {
        size_t bytes_read = ReadMemory (addr, prefer_file_cache, &uval, byte_size, error);
        if (bytes_read == byte_size)
        {
            DataExtractor data (&uval, sizeof(uval), m_arch.GetByteOrder(), m_arch.GetAddressByteSize());
            lldb::offset_t offset = 0;
            if (byte_size <= 4)
                scalar = data.GetMaxU32 (&offset, byte_size);
            else
                scalar = data.GetMaxU64 (&offset, byte_size);
            
            if (is_signed)
                scalar.SignExtend(byte_size * 8);
            return bytes_read;
        }
    }
    else
    {
        error.SetErrorStringWithFormat ("byte size of %u is too large for integer scalar type", byte_size);
    }
    return 0;
}

uint64_t
Target::ReadUnsignedIntegerFromMemory (const Address& addr, 
                                       bool prefer_file_cache,
                                       size_t integer_byte_size, 
                                       uint64_t fail_value, 
                                       Error &error)
{
    Scalar scalar;
    if (ReadScalarIntegerFromMemory (addr, 
                                     prefer_file_cache, 
                                     integer_byte_size, 
                                     false, 
                                     scalar, 
                                     error))
        return scalar.ULongLong(fail_value);
    return fail_value;
}

bool
Target::ReadPointerFromMemory (const Address& addr, 
                               bool prefer_file_cache,
                               Error &error,
                               Address &pointer_addr)
{
    Scalar scalar;
    if (ReadScalarIntegerFromMemory (addr, 
                                     prefer_file_cache, 
                                     m_arch.GetAddressByteSize(), 
                                     false, 
                                     scalar, 
                                     error))
    {
        addr_t pointer_vm_addr = scalar.ULongLong(LLDB_INVALID_ADDRESS);
        if (pointer_vm_addr != LLDB_INVALID_ADDRESS)
        {
            SectionLoadList &section_load_list = GetSectionLoadList();
            if (section_load_list.IsEmpty())
            {
                // No sections are loaded, so we must assume we are not running
                // yet and anything we are given is a file address.
                m_images.ResolveFileAddress (pointer_vm_addr, pointer_addr);
            }
            else
            {
                // We have at least one section loaded. This can be because
                // we have manually loaded some sections with "target modules load ..."
                // or because we have have a live process that has sections loaded
                // through the dynamic loader
                section_load_list.ResolveLoadAddress (pointer_vm_addr, pointer_addr);
            }
            // We weren't able to resolve the pointer value, so just return
            // an address with no section
            if (!pointer_addr.IsValid())
                pointer_addr.SetOffset (pointer_vm_addr);
            return true;
            
        }
    }
    return false;
}

ModuleSP
Target::GetSharedModule (const ModuleSpec &module_spec, Error *error_ptr)
{
    ModuleSP module_sp;

    Error error;

    // First see if we already have this module in our module list.  If we do, then we're done, we don't need
    // to consult the shared modules list.  But only do this if we are passed a UUID.
    
    if (module_spec.GetUUID().IsValid())
        module_sp = m_images.FindFirstModule(module_spec);
        
    if (!module_sp)
    {
        ModuleSP old_module_sp; // This will get filled in if we have a new version of the library
        bool did_create_module = false;
    
        // If there are image search path entries, try to use them first to acquire a suitable image.
        if (m_image_search_paths.GetSize())
        {
            ModuleSpec transformed_spec (module_spec);
            if (m_image_search_paths.RemapPath (module_spec.GetFileSpec().GetDirectory(), transformed_spec.GetFileSpec().GetDirectory()))
            {
                transformed_spec.GetFileSpec().GetFilename() = module_spec.GetFileSpec().GetFilename();
                error = ModuleList::GetSharedModule (transformed_spec, 
                                                     module_sp, 
                                                     &GetExecutableSearchPaths(),
                                                     &old_module_sp, 
                                                     &did_create_module);
            }
        }
        
        if (!module_sp)
        {
            // If we have a UUID, we can check our global shared module list in case
            // we already have it. If we don't have a valid UUID, then we can't since
            // the path in "module_spec" will be a platform path, and we will need to
            // let the platform find that file. For example, we could be asking for
            // "/usr/lib/dyld" and if we do not have a UUID, we don't want to pick
            // the local copy of "/usr/lib/dyld" since our platform could be a remote
            // platform that has its own "/usr/lib/dyld" in an SDK or in a local file
            // cache.
            if (module_spec.GetUUID().IsValid())
            {
                // We have a UUID, it is OK to check the global module list...
                error = ModuleList::GetSharedModule (module_spec,
                                                     module_sp, 
                                                     &GetExecutableSearchPaths(),
                                                     &old_module_sp,
                                                     &did_create_module);
            }

            if (!module_sp)
            {
                // The platform is responsible for finding and caching an appropriate
                // module in the shared module cache.
                if (m_platform_sp)
                {
                    error = m_platform_sp->GetSharedModule (module_spec,
                                                            m_process_sp.get(),
                                                            module_sp,
                                                            &GetExecutableSearchPaths(),
                                                            &old_module_sp,
                                                            &did_create_module);
                }
                else
                {
                    error.SetErrorString("no platform is currently set");
                }
            }
        }

        // We found a module that wasn't in our target list.  Let's make sure that there wasn't an equivalent
        // module in the list already, and if there was, let's remove it.
        if (module_sp)
        {
            ObjectFile *objfile = module_sp->GetObjectFile();
            if (objfile)
            {
                switch (objfile->GetType())
                {
                    case ObjectFile::eTypeCoreFile:      /// A core file that has a checkpoint of a program's execution state
                    case ObjectFile::eTypeExecutable:    /// A normal executable
                    case ObjectFile::eTypeDynamicLinker: /// The platform's dynamic linker executable
                    case ObjectFile::eTypeObjectFile:    /// An intermediate object file
                    case ObjectFile::eTypeSharedLibrary: /// A shared library that can be used during execution
                        break;
                    case ObjectFile::eTypeDebugInfo:     /// An object file that contains only debug information
                        if (error_ptr)
                            error_ptr->SetErrorString("debug info files aren't valid target modules, please specify an executable");
                        return ModuleSP();
                    case ObjectFile::eTypeStubLibrary:   /// A library that can be linked against but not used for execution
                        if (error_ptr)
                            error_ptr->SetErrorString("stub libraries aren't valid target modules, please specify an executable");
                        return ModuleSP();
                    default:
                        if (error_ptr)
                            error_ptr->SetErrorString("unsupported file type, please specify an executable");
                        return ModuleSP();
                }
                // GetSharedModule is not guaranteed to find the old shared module, for instance
                // in the common case where you pass in the UUID, it is only going to find the one
                // module matching the UUID.  In fact, it has no good way to know what the "old module"
                // relevant to this target is, since there might be many copies of a module with this file spec
                // in various running debug sessions, but only one of them will belong to this target.
                // So let's remove the UUID from the module list, and look in the target's module list.
                // Only do this if there is SOMETHING else in the module spec...
                if (!old_module_sp)
                {
                    if (module_spec.GetUUID().IsValid() && !module_spec.GetFileSpec().GetFilename().IsEmpty() && !module_spec.GetFileSpec().GetDirectory().IsEmpty())
                    {
                        ModuleSpec module_spec_copy(module_spec.GetFileSpec());
                        module_spec_copy.GetUUID().Clear();
                        
                        ModuleList found_modules;
                        size_t num_found = m_images.FindModules (module_spec_copy, found_modules);
                        if (num_found == 1)
                        {
                            old_module_sp = found_modules.GetModuleAtIndex(0);
                        }
                    }
                }
                
                if (old_module_sp && m_images.GetIndexForModule (old_module_sp.get()) != LLDB_INVALID_INDEX32)
                {
                    m_images.ReplaceModule(old_module_sp, module_sp);
                    Module *old_module_ptr = old_module_sp.get();
                    old_module_sp.reset();
                    ModuleList::RemoveSharedModuleIfOrphaned (old_module_ptr);
                }
                else
                    m_images.Append(module_sp);
            }
            else
                module_sp.reset();
        }
    }
    if (error_ptr)
        *error_ptr = error;
    return module_sp;
}

TargetSP
Target::CalculateTarget ()
{
    return shared_from_this();
}

ProcessSP
Target::CalculateProcess ()
{
    return m_process_sp;
}

ThreadSP
Target::CalculateThread ()
{
    return ThreadSP();
}

StackFrameSP
Target::CalculateStackFrame ()
{
    return StackFrameSP();
}

void
Target::CalculateExecutionContext (ExecutionContext &exe_ctx)
{
    exe_ctx.Clear();
    exe_ctx.SetTargetPtr(this);
}

PathMappingList &
Target::GetImageSearchPathList ()
{
    return m_image_search_paths;
}

void
Target::ImageSearchPathsChanged(const PathMappingList &path_list,
                                void *baton)
{
    Target *target = (Target *)baton;
    ModuleSP exe_module_sp (target->GetExecutableModule());
    if (exe_module_sp)
        target->SetExecutableModule (exe_module_sp, true);
}

TypeSystem *
Target::GetScratchTypeSystemForLanguage (Error *error, lldb::LanguageType language, bool create_on_demand)
{
    if (!m_valid)
        return nullptr;

    if (error)
    {
        error->Clear();
    }
    
    if (language == eLanguageTypeMipsAssembler // GNU AS and LLVM use it for all assembly code
        || language == eLanguageTypeUnknown)
    {
        std::set<lldb::LanguageType> languages_for_types;
        std::set<lldb::LanguageType> languages_for_expressions;
        
        Language::GetLanguagesSupportingTypeSystems(languages_for_types, languages_for_expressions);
        
        if (languages_for_expressions.count(eLanguageTypeC))
        {
            language = eLanguageTypeC; // LLDB's default.  Override by setting the target language.
        }
        else
        {
            if (languages_for_expressions.empty())
            {
                return nullptr;
            }
            else
            {
                language = *languages_for_expressions.begin();
            }
        }
    }

    return m_scratch_type_system_map.GetTypeSystemForLanguage(language, this, create_on_demand);
}

PersistentExpressionState *
Target::GetPersistentExpressionStateForLanguage (lldb::LanguageType language)
{
    TypeSystem *type_system = GetScratchTypeSystemForLanguage(nullptr, language, true);
    
    if (type_system)
    {
        return type_system->GetPersistentExpressionState();
    }
    else
    {
        return nullptr;
    }
}

UserExpression *
Target::GetUserExpressionForLanguage(const char *expr,
                                     const char *expr_prefix,
                                     lldb::LanguageType language,
                                     Expression::ResultType desired_type,
                                     const EvaluateExpressionOptions &options,
                                     Error &error)
{
    Error type_system_error;
    
    TypeSystem *type_system = GetScratchTypeSystemForLanguage (&type_system_error, language);
    UserExpression *user_expr = nullptr;
    
    if (!type_system)
    {
        error.SetErrorStringWithFormat("Could not find type system for language %s: %s", Language::GetNameForLanguageType(language), type_system_error.AsCString());
        return nullptr;
    }
    
    user_expr = type_system->GetUserExpression(expr, expr_prefix, language, desired_type, options);
    if (!user_expr)
        error.SetErrorStringWithFormat("Could not create an expression for language %s", Language::GetNameForLanguageType(language));
    
    return user_expr;
}

FunctionCaller *
Target::GetFunctionCallerForLanguage (lldb::LanguageType language,
                                      const CompilerType &return_type,
                                      const Address& function_address,
                                      const ValueList &arg_value_list,
                                      const char *name,
                                      Error &error)
{
    Error type_system_error;
    TypeSystem *type_system = GetScratchTypeSystemForLanguage (&type_system_error, language);
    FunctionCaller *persistent_fn = nullptr;
    
    if (!type_system)
    {
        error.SetErrorStringWithFormat("Could not find type system for language %s: %s", Language::GetNameForLanguageType(language), type_system_error.AsCString());
        return persistent_fn;
    }
    
    persistent_fn = type_system->GetFunctionCaller (return_type, function_address, arg_value_list, name);
    if (!persistent_fn)
        error.SetErrorStringWithFormat("Could not create an expression for language %s", Language::GetNameForLanguageType(language));
    
    return persistent_fn;
}

UtilityFunction *
Target::GetUtilityFunctionForLanguage (const char *text,
                                       lldb::LanguageType language,
                                       const char *name,
                                       Error &error)
{
    Error type_system_error;
    TypeSystem *type_system = GetScratchTypeSystemForLanguage (&type_system_error, language);
    UtilityFunction *utility_fn = nullptr;
    
    if (!type_system)
    {
        error.SetErrorStringWithFormat("Could not find type system for language %s: %s", Language::GetNameForLanguageType(language), type_system_error.AsCString());
        return utility_fn;
    }
    
    utility_fn = type_system->GetUtilityFunction (text, name);
    if (!utility_fn)
        error.SetErrorStringWithFormat("Could not create an expression for language %s", Language::GetNameForLanguageType(language));
    
    return utility_fn;
}

ClangASTContext *
Target::GetScratchClangASTContext(bool create_on_demand)
{
    if (m_valid)
    {
        if (TypeSystem* type_system = GetScratchTypeSystemForLanguage(nullptr, eLanguageTypeC, create_on_demand))
            return llvm::dyn_cast<ClangASTContext>(type_system);
    }
    return nullptr;
}

ClangASTImporterSP
Target::GetClangASTImporter()
{
    if (m_valid)
    {
        if (!m_ast_importer_sp)
        {
            m_ast_importer_sp.reset(new ClangASTImporter());
        }
        return m_ast_importer_sp;
    }
    return ClangASTImporterSP();
}

void
Target::SettingsInitialize ()
{
    Process::SettingsInitialize ();
}

void
Target::SettingsTerminate ()
{
    Process::SettingsTerminate ();
}

FileSpecList
Target::GetDefaultExecutableSearchPaths ()
{
    TargetPropertiesSP properties_sp(Target::GetGlobalProperties());
    if (properties_sp)
        return properties_sp->GetExecutableSearchPaths();
    return FileSpecList();
}

FileSpecList
Target::GetDefaultDebugFileSearchPaths ()
{
    TargetPropertiesSP properties_sp(Target::GetGlobalProperties());
    if (properties_sp)
        return properties_sp->GetDebugFileSearchPaths();
    return FileSpecList();
}

FileSpecList
Target::GetDefaultClangModuleSearchPaths ()
{
    TargetPropertiesSP properties_sp(Target::GetGlobalProperties());
    if (properties_sp)
        return properties_sp->GetClangModuleSearchPaths();
    return FileSpecList();
}

ArchSpec
Target::GetDefaultArchitecture ()
{
    TargetPropertiesSP properties_sp(Target::GetGlobalProperties());
    if (properties_sp)
        return properties_sp->GetDefaultArchitecture();
    return ArchSpec();
}

void
Target::SetDefaultArchitecture (const ArchSpec &arch)
{
    TargetPropertiesSP properties_sp(Target::GetGlobalProperties());
    if (properties_sp)
    {
        LogIfAnyCategoriesSet(LIBLLDB_LOG_TARGET, "Target::SetDefaultArchitecture setting target's default architecture to  %s (%s)", arch.GetArchitectureName(), arch.GetTriple().getTriple().c_str());
        return properties_sp->SetDefaultArchitecture(arch);
    }
}

Target *
Target::GetTargetFromContexts (const ExecutionContext *exe_ctx_ptr, const SymbolContext *sc_ptr)
{
    // The target can either exist in the "process" of ExecutionContext, or in 
    // the "target_sp" member of SymbolContext. This accessor helper function
    // will get the target from one of these locations.

    Target *target = nullptr;
    if (sc_ptr != nullptr)
        target = sc_ptr->target_sp.get();
    if (target == nullptr && exe_ctx_ptr)
        target = exe_ctx_ptr->GetTargetPtr();
    return target;
}

ExpressionResults
Target::EvaluateExpression(const char *expr_cstr,
                           ExecutionContextScope *exe_scope,
                           lldb::ValueObjectSP &result_valobj_sp,
                           const EvaluateExpressionOptions& options)
{
    result_valobj_sp.reset();
    
    ExpressionResults execution_results = eExpressionSetupError;

    if (expr_cstr == nullptr || expr_cstr[0] == '\0')
        return execution_results;

    // We shouldn't run stop hooks in expressions.
    // Be sure to reset this if you return anywhere within this function.
    bool old_suppress_value = m_suppress_stop_hooks;
    m_suppress_stop_hooks = true;

    ExecutionContext exe_ctx;
    
    if (exe_scope)
    {
        exe_scope->CalculateExecutionContext(exe_ctx);
    }
    else if (m_process_sp)
    {
        m_process_sp->CalculateExecutionContext(exe_ctx);
    }
    else
    {
        CalculateExecutionContext(exe_ctx);
    }
    
    // Make sure we aren't just trying to see the value of a persistent
    // variable (something like "$0")
    lldb::ExpressionVariableSP persistent_var_sp;
    // Only check for persistent variables the expression starts with a '$' 
    if (expr_cstr[0] == '$')
        persistent_var_sp = GetScratchTypeSystemForLanguage(nullptr, eLanguageTypeC)->GetPersistentExpressionState()->GetVariable (expr_cstr);

    if (persistent_var_sp)
    {
        result_valobj_sp = persistent_var_sp->GetValueObject ();
        execution_results = eExpressionCompleted;
    }
    else
    {
        const char *prefix = GetExpressionPrefixContentsAsCString();
        Error error;
        execution_results = UserExpression::Evaluate (exe_ctx,
                                                      options,
                                                      expr_cstr,
                                                      prefix,
                                                      result_valobj_sp,
                                                      error);
    }
    
    m_suppress_stop_hooks = old_suppress_value;
    
    return execution_results;
}

lldb::ExpressionVariableSP
Target::GetPersistentVariable(const ConstString &name)
{
    lldb::ExpressionVariableSP variable_sp;
    m_scratch_type_system_map.ForEach([this, name, &variable_sp](TypeSystem *type_system) -> bool
    {
        if (PersistentExpressionState *persistent_state = type_system->GetPersistentExpressionState())
        {
            variable_sp = persistent_state->GetVariable(name);

            if (variable_sp)
                return false;   // Stop iterating the ForEach
        }
        return true;    // Keep iterating the ForEach
    });
    return variable_sp;
}

lldb::addr_t
Target::GetPersistentSymbol(const ConstString &name)
{
    lldb::addr_t address = LLDB_INVALID_ADDRESS;
    
    m_scratch_type_system_map.ForEach([this, name, &address](TypeSystem *type_system) -> bool
    {
        if (PersistentExpressionState *persistent_state = type_system->GetPersistentExpressionState())
        {
            address = persistent_state->LookupSymbol(name);
            if (address != LLDB_INVALID_ADDRESS)
                return false;   // Stop iterating the ForEach
        }
        return true;    // Keep iterating the ForEach
    });
    return address;
}

lldb::addr_t
Target::GetCallableLoadAddress (lldb::addr_t load_addr, AddressClass addr_class) const
{
    addr_t code_addr = load_addr;
    switch (m_arch.GetMachine())
    {
    case llvm::Triple::mips:
    case llvm::Triple::mipsel:
    case llvm::Triple::mips64:
    case llvm::Triple::mips64el:
        switch (addr_class)
        {
        case eAddressClassData:
        case eAddressClassDebug:
            return LLDB_INVALID_ADDRESS;

        case eAddressClassUnknown:
        case eAddressClassInvalid:
        case eAddressClassCode:
        case eAddressClassCodeAlternateISA:
        case eAddressClassRuntime:
            if ((code_addr & 2ull) || (addr_class == eAddressClassCodeAlternateISA))
                code_addr |= 1ull;
            break;
        }
        break;

    case llvm::Triple::arm:
    case llvm::Triple::thumb:
        switch (addr_class)
        {
        case eAddressClassData:
        case eAddressClassDebug:
            return LLDB_INVALID_ADDRESS;
            
        case eAddressClassUnknown:
        case eAddressClassInvalid:
        case eAddressClassCode:
        case eAddressClassCodeAlternateISA:
        case eAddressClassRuntime:
            // Check if bit zero it no set?
            if ((code_addr & 1ull) == 0)
            {
                // Bit zero isn't set, check if the address is a multiple of 2?
                if (code_addr & 2ull)
                {
                    // The address is a multiple of 2 so it must be thumb, set bit zero
                    code_addr |= 1ull;
                }
                else if (addr_class == eAddressClassCodeAlternateISA)
                {
                    // We checked the address and the address claims to be the alternate ISA
                    // which means thumb, so set bit zero.
                    code_addr |= 1ull;
                }
            }
            break;
        }
        break;
            
    default:
        break;
    }
    return code_addr;
}

lldb::addr_t
Target::GetOpcodeLoadAddress (lldb::addr_t load_addr, AddressClass addr_class) const
{
    addr_t opcode_addr = load_addr;
    switch (m_arch.GetMachine())
    {
    case llvm::Triple::mips:
    case llvm::Triple::mipsel:
    case llvm::Triple::mips64:
    case llvm::Triple::mips64el:
    case llvm::Triple::arm:
    case llvm::Triple::thumb:
        switch (addr_class)
        {
        case eAddressClassData:
        case eAddressClassDebug:
            return LLDB_INVALID_ADDRESS;
            
        case eAddressClassInvalid:
        case eAddressClassUnknown:
        case eAddressClassCode:
        case eAddressClassCodeAlternateISA:
        case eAddressClassRuntime:
            opcode_addr &= ~(1ull);
            break;
        }
        break;
            
    default:
        break;
    }
    return opcode_addr;
}

lldb::addr_t
Target::GetBreakableLoadAddress (lldb::addr_t addr)
{
    addr_t breakable_addr = addr;
    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_BREAKPOINTS));

    switch (m_arch.GetMachine())
    {
    default:
        break;
    case llvm::Triple::mips:
    case llvm::Triple::mipsel:
    case llvm::Triple::mips64:
    case llvm::Triple::mips64el:
    {
        addr_t function_start = 0;
        addr_t current_offset = 0;
        uint32_t loop_count = 0;
        Address resolved_addr;
        uint32_t arch_flags = m_arch.GetFlags ();
        bool IsMips16 = arch_flags & ArchSpec::eMIPSAse_mips16;
        bool IsMicromips = arch_flags & ArchSpec::eMIPSAse_micromips;
        SectionLoadList &section_load_list = GetSectionLoadList();

        if (section_load_list.IsEmpty())
            // No sections are loaded, so we must assume we are not running yet
            // and need to operate only on file address.
            m_images.ResolveFileAddress (addr, resolved_addr); 
        else
            section_load_list.ResolveLoadAddress(addr, resolved_addr);

        // Get the function boundaries to make sure we don't scan back before the beginning of the current function.
        ModuleSP temp_addr_module_sp (resolved_addr.GetModule());
        if (temp_addr_module_sp)
        {
            SymbolContext sc;
            uint32_t resolve_scope = eSymbolContextFunction | eSymbolContextSymbol;
            temp_addr_module_sp->ResolveSymbolContextForAddress(resolved_addr, resolve_scope, sc);
            Address sym_addr;
            if (sc.function)
                sym_addr = sc.function->GetAddressRange().GetBaseAddress();
            else if (sc.symbol)
                sym_addr = sc.symbol->GetAddress();

            function_start = sym_addr.GetLoadAddress(this);
            if (function_start == LLDB_INVALID_ADDRESS)
                function_start = sym_addr.GetFileAddress();

            if (function_start)
                current_offset = addr - function_start;
        }

        // If breakpoint address is start of function then we dont have to do anything.
        if (current_offset == 0)
            return breakable_addr;
        else
            loop_count = current_offset / 2;

        if (loop_count > 3)
        {
            // Scan previous 6 bytes
            if (IsMips16 | IsMicromips)
                loop_count = 3;
            // For mips-only, instructions are always 4 bytes, so scan previous 4 bytes only.
            else
                loop_count = 2;
        }

        // Create Disassembler Instance
        lldb::DisassemblerSP disasm_sp(Disassembler::FindPlugin(m_arch, nullptr, nullptr));

        ExecutionContext exe_ctx;
        CalculateExecutionContext(exe_ctx);
        InstructionList instruction_list;
        InstructionSP prev_insn;
        bool prefer_file_cache = true; // Read from file
        uint32_t inst_to_choose = 0;

        for (uint32_t i = 1; i <= loop_count; i++)
        {
            // Adjust the address to read from.
            resolved_addr.Slide (-2);
            AddressRange range(resolved_addr, i*2);
            uint32_t insn_size = 0;

            disasm_sp->ParseInstructions(&exe_ctx, range, nullptr, prefer_file_cache);
            
            uint32_t num_insns = disasm_sp->GetInstructionList().GetSize();
            if (num_insns)
            {
                prev_insn = disasm_sp->GetInstructionList().GetInstructionAtIndex(0);
                insn_size = prev_insn->GetOpcode().GetByteSize();
                if (i == 1 && insn_size == 2)
                {
                    // This looks like a valid 2-byte instruction (but it could be a part of upper 4 byte instruction).
                    instruction_list.Append(prev_insn);
                    inst_to_choose = 1;
                }
                else if (i == 2)
                {
                    // Here we may get one 4-byte instruction or two 2-byte instructions.
                    if (num_insns == 2)
                    {
                        // Looks like there are two 2-byte instructions above our breakpoint target address.
                        // Now the upper 2-byte instruction is either a valid 2-byte instruction or could be a part of it's upper 4-byte instruction.
                        // In both cases we don't care because in this case lower 2-byte instruction is definitely a valid instruction
                        // and whatever i=1 iteration has found out is true.
                        inst_to_choose = 1;
                        break;
                    }
                    else if (insn_size == 4)
                    {
                        // This instruction claims its a valid 4-byte instruction. But it could be a part of it's upper 4-byte instruction.
                        // Lets try scanning upper 2 bytes to verify this.
                        instruction_list.Append(prev_insn);
                        inst_to_choose = 2;
                    }
                }
                else if (i == 3)
                {
                    if (insn_size == 4)
                        // FIXME: We reached here that means instruction at [target - 4] has already claimed to be a 4-byte instruction,
                        // and now instruction at [target - 6] is also claiming that it's a 4-byte instruction. This can not be true.
                        // In this case we can not decide the valid previous instruction so we let lldb set the breakpoint at the address given by user.
                        inst_to_choose = 0;
                    else
                        // This is straight-forward 
                        inst_to_choose = 2;
                    break;
                }
            }
            else
            {
                // Decode failed, bytes do not form a valid instruction. So whatever previous iteration has found out is true.
                if (i > 1)
                {
                    inst_to_choose = i - 1;
                    break;
                }
            }
        }

        // Check if we are able to find any valid instruction.
        if (inst_to_choose)
        {
            if (inst_to_choose > instruction_list.GetSize())
                inst_to_choose--;
            prev_insn = instruction_list.GetInstructionAtIndex(inst_to_choose - 1);

            if (prev_insn->HasDelaySlot())
            {
                uint32_t shift_size = prev_insn->GetOpcode().GetByteSize();
                // Adjust the breakable address
                breakable_addr = addr - shift_size;
                if (log)
                    log->Printf ("Target::%s Breakpoint at 0x%8.8" PRIx64 " is adjusted to 0x%8.8" PRIx64 " due to delay slot\n", __FUNCTION__, addr, breakable_addr);
            }
        }
        break;
    }
    }
    return breakable_addr;
}

SourceManager &
Target::GetSourceManager ()
{
    if (!m_source_manager_ap)
        m_source_manager_ap.reset (new SourceManager(shared_from_this()));
    return *m_source_manager_ap;
}

ClangModulesDeclVendor *
Target::GetClangModulesDeclVendor ()
{
    static Mutex s_clang_modules_decl_vendor_mutex; // If this is contended we can make it per-target
    
    {
        Mutex::Locker clang_modules_decl_vendor_locker(s_clang_modules_decl_vendor_mutex);
        
        if (!m_clang_modules_decl_vendor_ap)
        {
            m_clang_modules_decl_vendor_ap.reset(ClangModulesDeclVendor::Create(*this));
        }
    }
    
    return m_clang_modules_decl_vendor_ap.get();
}

Target::StopHookSP
Target::CreateStopHook ()
{
    lldb::user_id_t new_uid = ++m_stop_hook_next_id;
    Target::StopHookSP stop_hook_sp (new StopHook(shared_from_this(), new_uid));
    m_stop_hooks[new_uid] = stop_hook_sp;
    return stop_hook_sp;
}

bool
Target::RemoveStopHookByID (lldb::user_id_t user_id)
{
    size_t num_removed = m_stop_hooks.erase(user_id);
    return (num_removed != 0);
}

void
Target::RemoveAllStopHooks ()
{
    m_stop_hooks.clear();
}

Target::StopHookSP
Target::GetStopHookByID (lldb::user_id_t user_id)
{
    StopHookSP found_hook;
    
    StopHookCollection::iterator specified_hook_iter;
    specified_hook_iter = m_stop_hooks.find (user_id);
    if (specified_hook_iter != m_stop_hooks.end())
        found_hook = (*specified_hook_iter).second;
    return found_hook;
}

bool
Target::SetStopHookActiveStateByID (lldb::user_id_t user_id, bool active_state)
{
    StopHookCollection::iterator specified_hook_iter;
    specified_hook_iter = m_stop_hooks.find (user_id);
    if (specified_hook_iter == m_stop_hooks.end())
        return false;
        
    (*specified_hook_iter).second->SetIsActive (active_state);
    return true;
}

void
Target::SetAllStopHooksActiveState (bool active_state)
{
    StopHookCollection::iterator pos, end = m_stop_hooks.end();
    for (pos = m_stop_hooks.begin(); pos != end; pos++)
    {
        (*pos).second->SetIsActive (active_state);
    }
}

void
Target::RunStopHooks ()
{
    if (m_suppress_stop_hooks)
        return;
        
    if (!m_process_sp)
        return;
    
    // <rdar://problem/12027563> make sure we check that we are not stopped because of us running a user expression
    // since in that case we do not want to run the stop-hooks
    if (m_process_sp->GetModIDRef().IsLastResumeForUserExpression())
        return;
    
    if (m_stop_hooks.empty())
        return;
        
    StopHookCollection::iterator pos, end = m_stop_hooks.end();
        
    // If there aren't any active stop hooks, don't bother either:
    bool any_active_hooks = false;
    for (pos = m_stop_hooks.begin(); pos != end; pos++)
    {
        if ((*pos).second->IsActive())
        {
            any_active_hooks = true;
            break;
        }
    }
    if (!any_active_hooks)
        return;
    
    CommandReturnObject result;
    
    std::vector<ExecutionContext> exc_ctx_with_reasons;
    std::vector<SymbolContext> sym_ctx_with_reasons;
    
    ThreadList &cur_threadlist = m_process_sp->GetThreadList();
    size_t num_threads = cur_threadlist.GetSize();
    for (size_t i = 0; i < num_threads; i++)
    {
        lldb::ThreadSP cur_thread_sp = cur_threadlist.GetThreadAtIndex (i);
        if (cur_thread_sp->ThreadStoppedForAReason())
        {
            lldb::StackFrameSP cur_frame_sp = cur_thread_sp->GetStackFrameAtIndex(0);
            exc_ctx_with_reasons.push_back(ExecutionContext(m_process_sp.get(), cur_thread_sp.get(), cur_frame_sp.get()));
            sym_ctx_with_reasons.push_back(cur_frame_sp->GetSymbolContext(eSymbolContextEverything));
        }
    }
    
    // If no threads stopped for a reason, don't run the stop-hooks.
    size_t num_exe_ctx = exc_ctx_with_reasons.size();
    if (num_exe_ctx == 0)
        return;
    
    result.SetImmediateOutputStream (m_debugger.GetAsyncOutputStream());
    result.SetImmediateErrorStream (m_debugger.GetAsyncErrorStream());
    
    bool keep_going = true;
    bool hooks_ran = false;
    bool print_hook_header = (m_stop_hooks.size() != 1);
    bool print_thread_header = (num_exe_ctx != 1);
    
    for (pos = m_stop_hooks.begin(); keep_going && pos != end; pos++)
    {
        // result.Clear();
        StopHookSP cur_hook_sp = (*pos).second;
        if (!cur_hook_sp->IsActive())
            continue;
        
        bool any_thread_matched = false;
        for (size_t i = 0; keep_going && i < num_exe_ctx; i++)
        {
            if ((cur_hook_sp->GetSpecifier() == nullptr 
                  || cur_hook_sp->GetSpecifier()->SymbolContextMatches(sym_ctx_with_reasons[i]))
                && (cur_hook_sp->GetThreadSpecifier() == nullptr
                    || cur_hook_sp->GetThreadSpecifier()->ThreadPassesBasicTests(exc_ctx_with_reasons[i].GetThreadRef())))
            {
                if (!hooks_ran)
                {
                    hooks_ran = true;
                }
                if (print_hook_header && !any_thread_matched)
                {
                    const char *cmd = (cur_hook_sp->GetCommands().GetSize() == 1 ?
                                       cur_hook_sp->GetCommands().GetStringAtIndex(0) :
                                       nullptr);
                    if (cmd)
                        result.AppendMessageWithFormat("\n- Hook %" PRIu64 " (%s)\n", cur_hook_sp->GetID(), cmd);
                    else
                        result.AppendMessageWithFormat("\n- Hook %" PRIu64 "\n", cur_hook_sp->GetID());
                    any_thread_matched = true;
                }
                
                if (print_thread_header)
                    result.AppendMessageWithFormat("-- Thread %d\n", exc_ctx_with_reasons[i].GetThreadPtr()->GetIndexID());

                CommandInterpreterRunOptions options;
                options.SetStopOnContinue (true);
                options.SetStopOnError (true);
                options.SetEchoCommands (false);
                options.SetPrintResults (true);
                options.SetAddToHistory (false);

                GetDebugger().GetCommandInterpreter().HandleCommands (cur_hook_sp->GetCommands(),
                                                                      &exc_ctx_with_reasons[i],
                                                                      options,
                                                                      result);

                // If the command started the target going again, we should bag out of
                // running the stop hooks.
                if ((result.GetStatus() == eReturnStatusSuccessContinuingNoResult) || 
                    (result.GetStatus() == eReturnStatusSuccessContinuingResult))
                {
                    result.AppendMessageWithFormat ("Aborting stop hooks, hook %" PRIu64 " set the program running.", cur_hook_sp->GetID());
                    keep_going = false;
                }
            }
        }
    }

    result.GetImmediateOutputStream()->Flush();
    result.GetImmediateErrorStream()->Flush();
}

const TargetPropertiesSP &
Target::GetGlobalProperties()
{
    // NOTE: intentional leak so we don't crash if global destructor chain gets
    // called as other threads still use the result of this function
    static TargetPropertiesSP *g_settings_sp_ptr = nullptr;
    static std::once_flag g_once_flag;
    std::call_once(g_once_flag,  []() {
        g_settings_sp_ptr = new TargetPropertiesSP(new TargetProperties(nullptr));
    });
    return *g_settings_sp_ptr;
}

Error
Target::Install (ProcessLaunchInfo *launch_info)
{
    Error error;
    PlatformSP platform_sp (GetPlatform());
    if (platform_sp)
    {
        if (platform_sp->IsRemote())
        {
            if (platform_sp->IsConnected())
            {
                // Install all files that have an install path, and always install the
                // main executable when connected to a remote platform
                const ModuleList& modules = GetImages();
                const size_t num_images = modules.GetSize();
                for (size_t idx = 0; idx < num_images; ++idx)
                {
                    const bool is_main_executable = idx == 0;
                    ModuleSP module_sp(modules.GetModuleAtIndex(idx));
                    if (module_sp)
                    {
                        FileSpec local_file (module_sp->GetFileSpec());
                        if (local_file)
                        {
                            FileSpec remote_file (module_sp->GetRemoteInstallFileSpec());
                            if (!remote_file)
                            {
                                if (is_main_executable) // TODO: add setting for always installing main executable???
                                {
                                    // Always install the main executable
                                    remote_file = platform_sp->GetRemoteWorkingDirectory();
                                    remote_file.AppendPathComponent(module_sp->GetFileSpec().GetFilename().GetCString());
                                }
                            }
                            if (remote_file)
                            {
                                error = platform_sp->Install(local_file, remote_file);
                                if (error.Success())
                                {
                                    module_sp->SetPlatformFileSpec(remote_file);
                                    if (is_main_executable)
                                    {
                                        platform_sp->SetFilePermissions(remote_file, 0700);
                                        if (launch_info)
                                            launch_info->SetExecutableFile(remote_file, false);
                                    }
                                }
                                else
                                    break;
                            }
                        }
                    }
                }
            }
        }
    }
    return error;
}

bool
Target::ResolveLoadAddress (addr_t load_addr, Address &so_addr, uint32_t stop_id)
{
    return m_section_load_history.ResolveLoadAddress(stop_id, load_addr, so_addr);
}

bool
Target::ResolveFileAddress (lldb::addr_t file_addr, Address &resolved_addr)
{
    return m_images.ResolveFileAddress(file_addr, resolved_addr);
}

bool
Target::SetSectionLoadAddress (const SectionSP &section_sp, addr_t new_section_load_addr, bool warn_multiple)
{
    const addr_t old_section_load_addr = m_section_load_history.GetSectionLoadAddress (SectionLoadHistory::eStopIDNow, section_sp);
    if (old_section_load_addr != new_section_load_addr)
    {
        uint32_t stop_id = 0;
        ProcessSP process_sp(GetProcessSP());
        if (process_sp)
            stop_id = process_sp->GetStopID();
        else
            stop_id = m_section_load_history.GetLastStopID();
        if (m_section_load_history.SetSectionLoadAddress (stop_id, section_sp, new_section_load_addr, warn_multiple))
            return true; // Return true if the section load address was changed...
    }
    return false; // Return false to indicate nothing changed
}

size_t
Target::UnloadModuleSections (const ModuleList &module_list)
{
    size_t section_unload_count = 0;
    size_t num_modules = module_list.GetSize();
    for (size_t i=0; i<num_modules; ++i)
    {
        section_unload_count += UnloadModuleSections (module_list.GetModuleAtIndex(i));
    }
    return section_unload_count;
}

size_t
Target::UnloadModuleSections (const lldb::ModuleSP &module_sp)
{
    uint32_t stop_id = 0;
    ProcessSP process_sp(GetProcessSP());
    if (process_sp)
        stop_id = process_sp->GetStopID();
    else
        stop_id = m_section_load_history.GetLastStopID();
    SectionList *sections = module_sp->GetSectionList();
    size_t section_unload_count = 0;
    if (sections)
    {
        const uint32_t num_sections = sections->GetNumSections(0);
        for (uint32_t i = 0; i < num_sections; ++i)
        {
            section_unload_count += m_section_load_history.SetSectionUnloaded(stop_id, sections->GetSectionAtIndex(i));
        }
    }
    return section_unload_count;
}

bool
Target::SetSectionUnloaded (const lldb::SectionSP &section_sp)
{
    uint32_t stop_id = 0;
    ProcessSP process_sp(GetProcessSP());
    if (process_sp)
        stop_id = process_sp->GetStopID();
    else
        stop_id = m_section_load_history.GetLastStopID();
    return m_section_load_history.SetSectionUnloaded (stop_id, section_sp);
}

bool
Target::SetSectionUnloaded (const lldb::SectionSP &section_sp, addr_t load_addr)
{
    uint32_t stop_id = 0;
    ProcessSP process_sp(GetProcessSP());
    if (process_sp)
        stop_id = process_sp->GetStopID();
    else
        stop_id = m_section_load_history.GetLastStopID();
    return m_section_load_history.SetSectionUnloaded (stop_id, section_sp, load_addr);
}

void
Target::ClearAllLoadedSections ()
{
    m_section_load_history.Clear();
}

Error
Target::Launch (ProcessLaunchInfo &launch_info, Stream *stream)
{
    Error error;
    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_TARGET));

    if (log)
        log->Printf ("Target::%s() called for %s", __FUNCTION__, launch_info.GetExecutableFile().GetPath().c_str ());

    StateType state = eStateInvalid;
    
    // Scope to temporarily get the process state in case someone has manually
    // remotely connected already to a process and we can skip the platform
    // launching.
    {
        ProcessSP process_sp (GetProcessSP());
    
        if (process_sp)
        {
            state = process_sp->GetState();
            if (log)
                log->Printf ("Target::%s the process exists, and its current state is %s", __FUNCTION__, StateAsCString (state));
        }
        else
        {
            if (log)
                log->Printf ("Target::%s the process instance doesn't currently exist.", __FUNCTION__);
        }
    }

    launch_info.GetFlags().Set (eLaunchFlagDebug);
    
    // Get the value of synchronous execution here.  If you wait till after you have started to
    // run, then you could have hit a breakpoint, whose command might switch the value, and
    // then you'll pick up that incorrect value.
    Debugger &debugger = GetDebugger();
    const bool synchronous_execution = debugger.GetCommandInterpreter().GetSynchronous ();
    
    PlatformSP platform_sp (GetPlatform());
    
    // Finalize the file actions, and if none were given, default to opening
    // up a pseudo terminal
    const bool default_to_use_pty = platform_sp ? platform_sp->IsHost() : false;
    if (log)
        log->Printf ("Target::%s have platform=%s, platform_sp->IsHost()=%s, default_to_use_pty=%s",
                     __FUNCTION__,
                     platform_sp ? "true" : "false",
                     platform_sp ? (platform_sp->IsHost () ? "true" : "false") : "n/a",
                     default_to_use_pty ? "true" : "false");

    launch_info.FinalizeFileActions (this, default_to_use_pty);
    
    if (state == eStateConnected)
    {
        if (launch_info.GetFlags().Test (eLaunchFlagLaunchInTTY))
        {
            error.SetErrorString("can't launch in tty when launching through a remote connection");
            return error;
        }
    }
    
    if (!launch_info.GetArchitecture().IsValid())
        launch_info.GetArchitecture() = GetArchitecture();

    // If we're not already connected to the process, and if we have a platform that can launch a process for debugging, go ahead and do that here.
    if (state != eStateConnected && platform_sp && platform_sp->CanDebugProcess ())
    {
        if (log)
            log->Printf ("Target::%s asking the platform to debug the process", __FUNCTION__);

        // Get a weak pointer to the previous process if we have one
        ProcessWP process_wp;
        if (m_process_sp)
            process_wp = m_process_sp;
        m_process_sp = GetPlatform()->DebugProcess (launch_info,
                                                    debugger,
                                                    this,
                                                    error);

        // Cleanup the old process since someone might still have a strong
        // reference to this process and we would like to allow it to cleanup
        // as much as it can without the object being destroyed. We try to
        // lock the shared pointer and if that works, then someone else still
        // has a strong reference to the process.

        ProcessSP old_process_sp(process_wp.lock());
        if (old_process_sp)
            old_process_sp->Finalize();
    }
    else
    {
        if (log)
            log->Printf ("Target::%s the platform doesn't know how to debug a process, getting a process plugin to do this for us.", __FUNCTION__);

        if (state == eStateConnected)
        {
            assert(m_process_sp);
        }
        else
        {
            // Use a Process plugin to construct the process.
            const char *plugin_name = launch_info.GetProcessPluginName();
            CreateProcess(launch_info.GetListenerForProcess(debugger), plugin_name, nullptr);
        }

        // Since we didn't have a platform launch the process, launch it here.
        if (m_process_sp)
            error = m_process_sp->Launch (launch_info);
    }
    
    if (!m_process_sp)
    {
        if (error.Success())
            error.SetErrorString("failed to launch or debug process");
        return error;
    }

    if (error.Success())
    {
        if (synchronous_execution || !launch_info.GetFlags().Test(eLaunchFlagStopAtEntry))
        {
            ListenerSP hijack_listener_sp (launch_info.GetHijackListener());
            if (!hijack_listener_sp)
            {
                hijack_listener_sp = Listener::MakeListener("lldb.Target.Launch.hijack");
                launch_info.SetHijackListener(hijack_listener_sp);
                m_process_sp->HijackProcessEvents(hijack_listener_sp);
            }

            StateType state = m_process_sp->WaitForProcessToStop(nullptr, nullptr, false, hijack_listener_sp, nullptr);
            
            if (state == eStateStopped)
            {
                if (!launch_info.GetFlags().Test(eLaunchFlagStopAtEntry))
                {
                    if (synchronous_execution)
                    {
                        error = m_process_sp->PrivateResume();
                        if (error.Success())
                        {
                            state = m_process_sp->WaitForProcessToStop(nullptr, nullptr, true, hijack_listener_sp, stream);
                            const bool must_be_alive = false; // eStateExited is ok, so this must be false
                            if (!StateIsStoppedState(state, must_be_alive))
                            {
                                error.SetErrorStringWithFormat("process isn't stopped: %s", StateAsCString(state));
                            }
                        }
                    }
                    else
                    {
                        m_process_sp->RestoreProcessEvents();
                        error = m_process_sp->PrivateResume();
                    }
                    if (!error.Success())
                    {
                        Error error2;
                        error2.SetErrorStringWithFormat("process resume at entry point failed: %s", error.AsCString());
                        error = error2;
                    }
                }
            }
            else if (state == eStateExited)
            {
                bool with_shell = !!launch_info.GetShell();
                const int exit_status = m_process_sp->GetExitStatus();
                const char *exit_desc = m_process_sp->GetExitDescription();
#define LAUNCH_SHELL_MESSAGE "\n'r' and 'run' are aliases that default to launching through a shell.\nTry launching without going through a shell by using 'process launch'."
                if (exit_desc && exit_desc[0])
                {
                    if (with_shell)
                        error.SetErrorStringWithFormat ("process exited with status %i (%s)" LAUNCH_SHELL_MESSAGE, exit_status, exit_desc);
                    else
                        error.SetErrorStringWithFormat ("process exited with status %i (%s)", exit_status, exit_desc);
                }
                else
                {
                    if (with_shell)
                        error.SetErrorStringWithFormat ("process exited with status %i" LAUNCH_SHELL_MESSAGE, exit_status);
                    else
                        error.SetErrorStringWithFormat ("process exited with status %i", exit_status);
                }
            }
            else
            {
                error.SetErrorStringWithFormat ("initial process state wasn't stopped: %s", StateAsCString(state));
            }
        }
        m_process_sp->RestoreProcessEvents ();
    }
    else
    {
        Error error2;
        error2.SetErrorStringWithFormat ("process launch failed: %s", error.AsCString());
        error = error2;
    }
    return error;
}

Error
Target::Attach (ProcessAttachInfo &attach_info, Stream *stream)
{
    auto state = eStateInvalid;
    auto process_sp = GetProcessSP ();
    if (process_sp)
    {
        state = process_sp->GetState ();
        if (process_sp->IsAlive () && state != eStateConnected)
        {
            if (state == eStateAttaching)
                return Error ("process attach is in progress");
            return Error ("a process is already being debugged");
        }
    }

    const ModuleSP old_exec_module_sp = GetExecutableModule ();

    // If no process info was specified, then use the target executable
    // name as the process to attach to by default
    if (!attach_info.ProcessInfoSpecified ())
    {
        if (old_exec_module_sp)
            attach_info.GetExecutableFile ().GetFilename () = old_exec_module_sp->GetPlatformFileSpec ().GetFilename ();

        if (!attach_info.ProcessInfoSpecified ())
        {
            return Error ("no process specified, create a target with a file, or specify the --pid or --name");
        }
    }

    const auto platform_sp = GetDebugger ().GetPlatformList ().GetSelectedPlatform ();
    ListenerSP hijack_listener_sp;
    const bool async = attach_info.GetAsync();
    if (!async)
    {
        hijack_listener_sp = Listener::MakeListener("lldb.Target.Attach.attach.hijack");
        attach_info.SetHijackListener (hijack_listener_sp);
    }

    Error error;
    if (state != eStateConnected && platform_sp != nullptr && platform_sp->CanDebugProcess ())
    {
        SetPlatform (platform_sp);
        process_sp = platform_sp->Attach (attach_info, GetDebugger (), this, error);
    }
    else
    {
        if (state != eStateConnected)
        {
            const char *plugin_name = attach_info.GetProcessPluginName ();
            process_sp = CreateProcess (attach_info.GetListenerForProcess (GetDebugger ()), plugin_name, nullptr);
            if (process_sp == nullptr)
            {
                error.SetErrorStringWithFormat ("failed to create process using plugin %s", (plugin_name) ? plugin_name : "null");
                return error;
            }
        }
        if (hijack_listener_sp)
            process_sp->HijackProcessEvents (hijack_listener_sp);
        error = process_sp->Attach (attach_info);
    }

    if (error.Success () && process_sp)
    {
        if (async)
        {
            process_sp->RestoreProcessEvents ();
        }
        else
        {
            state = process_sp->WaitForProcessToStop (nullptr, nullptr, false, attach_info.GetHijackListener(), stream);
            process_sp->RestoreProcessEvents ();

            if (state != eStateStopped)
            {
                const char *exit_desc = process_sp->GetExitDescription ();
                if (exit_desc)
                    error.SetErrorStringWithFormat ("%s", exit_desc);
                else
                    error.SetErrorString ("process did not stop (no such process or permission problem?)");
                process_sp->Destroy (false);
            }
        }
    }
    return error;
}

//--------------------------------------------------------------
// Target::StopHook
//--------------------------------------------------------------
Target::StopHook::StopHook (lldb::TargetSP target_sp, lldb::user_id_t uid) :
        UserID (uid),
        m_target_sp (target_sp),
        m_commands (),
        m_specifier_sp (),
        m_thread_spec_ap(),
        m_active (true)
{
}

Target::StopHook::StopHook (const StopHook &rhs) :
        UserID (rhs.GetID()),
        m_target_sp (rhs.m_target_sp),
        m_commands (rhs.m_commands),
        m_specifier_sp (rhs.m_specifier_sp),
        m_thread_spec_ap (),
        m_active (rhs.m_active)
{
    if (rhs.m_thread_spec_ap)
        m_thread_spec_ap.reset (new ThreadSpec(*rhs.m_thread_spec_ap.get()));
}
        
Target::StopHook::~StopHook() = default;

void
Target::StopHook::SetSpecifier(SymbolContextSpecifier *specifier)
{
    m_specifier_sp.reset(specifier);
}

void
Target::StopHook::SetThreadSpecifier (ThreadSpec *specifier)
{
    m_thread_spec_ap.reset (specifier);
}

void
Target::StopHook::GetDescription (Stream *s, lldb::DescriptionLevel level) const
{
    int indent_level = s->GetIndentLevel();

    s->SetIndentLevel(indent_level + 2);

    s->Printf ("Hook: %" PRIu64 "\n", GetID());
    if (m_active)
        s->Indent ("State: enabled\n");
    else
        s->Indent ("State: disabled\n");    
    
    if (m_specifier_sp)
    {
        s->Indent();
        s->PutCString ("Specifier:\n");
        s->SetIndentLevel (indent_level + 4);
        m_specifier_sp->GetDescription (s, level);
        s->SetIndentLevel (indent_level + 2);
    }

    if (m_thread_spec_ap)
    {
        StreamString tmp;
        s->Indent("Thread:\n");
        m_thread_spec_ap->GetDescription (&tmp, level);
        s->SetIndentLevel (indent_level + 4);
        s->Indent (tmp.GetData());
        s->PutCString ("\n");
        s->SetIndentLevel (indent_level + 2);
    }

    s->Indent ("Commands: \n");
    s->SetIndentLevel (indent_level + 4);
    uint32_t num_commands = m_commands.GetSize();
    for (uint32_t i = 0; i < num_commands; i++)
    {
        s->Indent(m_commands.GetStringAtIndex(i));
        s->PutCString ("\n");
    }
    s->SetIndentLevel (indent_level);
}

//--------------------------------------------------------------
// class TargetProperties
//--------------------------------------------------------------

OptionEnumValueElement
lldb_private::g_dynamic_value_types[] =
{
    { eNoDynamicValues,      "no-dynamic-values", "Don't calculate the dynamic type of values"},
    { eDynamicCanRunTarget,  "run-target",        "Calculate the dynamic type of values even if you have to run the target."},
    { eDynamicDontRunTarget, "no-run-target",     "Calculate the dynamic type of values, but don't run the target."},
    { 0, nullptr, nullptr }
};

static OptionEnumValueElement
g_inline_breakpoint_enums[] =
{
    { eInlineBreakpointsNever,   "never",     "Never look for inline breakpoint locations (fastest). This setting should only be used if you know that no inlining occurs in your programs."},
    { eInlineBreakpointsHeaders, "headers",   "Only check for inline breakpoint locations when setting breakpoints in header files, but not when setting breakpoint in implementation source files (default)."},
    { eInlineBreakpointsAlways,  "always",    "Always look for inline breakpoint locations when setting file and line breakpoints (slower but most accurate)."},
    { 0, nullptr, nullptr }
};

typedef enum x86DisassemblyFlavor
{
    eX86DisFlavorDefault,
    eX86DisFlavorIntel,
    eX86DisFlavorATT
} x86DisassemblyFlavor;

static OptionEnumValueElement
g_x86_dis_flavor_value_types[] =
{
    { eX86DisFlavorDefault, "default", "Disassembler default (currently att)."},
    { eX86DisFlavorIntel,   "intel",   "Intel disassembler flavor."},
    { eX86DisFlavorATT,     "att",     "AT&T disassembler flavor."},
    { 0, nullptr, nullptr }
};

static OptionEnumValueElement
g_hex_immediate_style_values[] =
{
    { Disassembler::eHexStyleC,        "c",      "C-style (0xffff)."},
    { Disassembler::eHexStyleAsm,      "asm",    "Asm-style (0ffffh)."},
    { 0, nullptr, nullptr }
};

static OptionEnumValueElement
g_load_script_from_sym_file_values[] =
{
    { eLoadScriptFromSymFileTrue,    "true",    "Load debug scripts inside symbol files"},
    { eLoadScriptFromSymFileFalse,   "false",   "Do not load debug scripts inside symbol files."},
    { eLoadScriptFromSymFileWarn,    "warn",    "Warn about debug scripts inside symbol files but do not load them."},
    { 0, nullptr, nullptr }
};

static OptionEnumValueElement
g_load_current_working_dir_lldbinit_values[] =
{
    { eLoadCWDlldbinitTrue,    "true",    "Load .lldbinit files from current directory"},
    { eLoadCWDlldbinitFalse,   "false",   "Do not load .lldbinit files from current directory"},
    { eLoadCWDlldbinitWarn,    "warn",    "Warn about loading .lldbinit files from current directory"},
    { 0, nullptr, nullptr }
};

static OptionEnumValueElement
g_memory_module_load_level_values[] =
{
    { eMemoryModuleLoadLevelMinimal,  "minimal" , "Load minimal information when loading modules from memory. Currently this setting loads sections only."},
    { eMemoryModuleLoadLevelPartial,  "partial" , "Load partial information when loading modules from memory. Currently this setting loads sections and function bounds."},
    { eMemoryModuleLoadLevelComplete, "complete", "Load complete information when loading modules from memory. Currently this setting loads sections and all symbols."},
    { 0, nullptr, nullptr }
};

static PropertyDefinition
g_properties[] =
{
    { "default-arch"                       , OptionValue::eTypeArch      , true , 0                         , nullptr, nullptr, "Default architecture to choose, when there's a choice." },
    { "move-to-nearest-code"               , OptionValue::eTypeBoolean   , false, true                      , nullptr, nullptr, "Move breakpoints to nearest code." },
    { "language"                           , OptionValue::eTypeLanguage  , false, eLanguageTypeUnknown      , nullptr, nullptr, "The language to use when interpreting expressions entered in commands." },
    { "expr-prefix"                        , OptionValue::eTypeFileSpec  , false, 0                         , nullptr, nullptr, "Path to a file containing expressions to be prepended to all expressions." },
    { "prefer-dynamic-value"               , OptionValue::eTypeEnum      , false, eDynamicDontRunTarget     , nullptr, g_dynamic_value_types, "Should printed values be shown as their dynamic value." },
    { "enable-synthetic-value"             , OptionValue::eTypeBoolean   , false, true                      , nullptr, nullptr, "Should synthetic values be used by default whenever available." },
    { "skip-prologue"                      , OptionValue::eTypeBoolean   , false, true                      , nullptr, nullptr, "Skip function prologues when setting breakpoints by name." },
    { "source-map"                         , OptionValue::eTypePathMap   , false, 0                         , nullptr, nullptr, "Source path remappings are used to track the change of location between a source file when built, and "
      "where it exists on the current system.  It consists of an array of duples, the first element of each duple is "
      "some part (starting at the root) of the path to the file when it was built, "
      "and the second is where the remainder of the original build hierarchy is rooted on the local system.  "
      "Each element of the array is checked in order and the first one that results in a match wins." },
    { "exec-search-paths"                  , OptionValue::eTypeFileSpecList, false, 0                       , nullptr, nullptr, "Executable search paths to use when locating executable files whose paths don't match the local file system." },
    { "debug-file-search-paths"            , OptionValue::eTypeFileSpecList, false, 0                       , nullptr, nullptr, "List of directories to be searched when locating debug symbol files." },
    { "clang-module-search-paths"          , OptionValue::eTypeFileSpecList, false, 0                       , nullptr, nullptr, "List of directories to be searched when locating modules for Clang." },
    { "auto-import-clang-modules"          , OptionValue::eTypeBoolean   , false, true                      , nullptr, nullptr, "Automatically load Clang modules referred to by the program." },
    { "max-children-count"                 , OptionValue::eTypeSInt64    , false, 256                       , nullptr, nullptr, "Maximum number of children to expand in any level of depth." },
    { "max-string-summary-length"          , OptionValue::eTypeSInt64    , false, 1024                      , nullptr, nullptr, "Maximum number of characters to show when using %s in summary strings." },
    { "max-memory-read-size"               , OptionValue::eTypeSInt64    , false, 1024                      , nullptr, nullptr, "Maximum number of bytes that 'memory read' will fetch before --force must be specified." },
    { "breakpoints-use-platform-avoid-list", OptionValue::eTypeBoolean   , false, true                      , nullptr, nullptr, "Consult the platform module avoid list when setting non-module specific breakpoints." },
    { "arg0"                               , OptionValue::eTypeString    , false, 0                         , nullptr, nullptr, "The first argument passed to the program in the argument array which can be different from the executable itself." },
    { "run-args"                           , OptionValue::eTypeArgs      , false, 0                         , nullptr, nullptr, "A list containing all the arguments to be passed to the executable when it is run. Note that this does NOT include the argv[0] which is in target.arg0." },
    { "env-vars"                           , OptionValue::eTypeDictionary, false, OptionValue::eTypeString  , nullptr, nullptr, "A list of all the environment variables to be passed to the executable's environment, and their values." },
    { "inherit-env"                        , OptionValue::eTypeBoolean   , false, true                      , nullptr, nullptr, "Inherit the environment from the process that is running LLDB." },
    { "input-path"                         , OptionValue::eTypeFileSpec  , false, 0                         , nullptr, nullptr, "The file/path to be used by the executable program for reading its standard input." },
    { "output-path"                        , OptionValue::eTypeFileSpec  , false, 0                         , nullptr, nullptr, "The file/path to be used by the executable program for writing its standard output." },
    { "error-path"                         , OptionValue::eTypeFileSpec  , false, 0                         , nullptr, nullptr, "The file/path to be used by the executable program for writing its standard error." },
    { "detach-on-error"                    , OptionValue::eTypeBoolean   , false, true                      , nullptr, nullptr, "debugserver will detach (rather than killing) a process if it loses connection with lldb." },
    { "disable-aslr"                       , OptionValue::eTypeBoolean   , false, true                      , nullptr, nullptr, "Disable Address Space Layout Randomization (ASLR)" },
    { "disable-stdio"                      , OptionValue::eTypeBoolean   , false, false                     , nullptr, nullptr, "Disable stdin/stdout for process (e.g. for a GUI application)" },
    { "inline-breakpoint-strategy"         , OptionValue::eTypeEnum      , false, eInlineBreakpointsAlways  , nullptr, g_inline_breakpoint_enums, "The strategy to use when settings breakpoints by file and line. "
        "Breakpoint locations can end up being inlined by the compiler, so that a compile unit 'a.c' might contain an inlined function from another source file. "
        "Usually this is limited to breakpoint locations from inlined functions from header or other include files, or more accurately non-implementation source files. "
        "Sometimes code might #include implementation files and cause inlined breakpoint locations in inlined implementation files. "
        "Always checking for inlined breakpoint locations can be expensive (memory and time), so if you have a project with many headers "
        "and find that setting breakpoints is slow, then you can change this setting to headers. "
        "This setting allows you to control exactly which strategy is used when setting "
        "file and line breakpoints." },
    // FIXME: This is the wrong way to do per-architecture settings, but we don't have a general per architecture settings system in place yet.
    { "x86-disassembly-flavor"             , OptionValue::eTypeEnum      , false, eX86DisFlavorDefault,       nullptr, g_x86_dis_flavor_value_types, "The default disassembly flavor to use for x86 or x86-64 targets." },
    { "use-hex-immediates"                 , OptionValue::eTypeBoolean   , false, true,                       nullptr, nullptr, "Show immediates in disassembly as hexadecimal." },
    { "hex-immediate-style"                , OptionValue::eTypeEnum   ,    false, Disassembler::eHexStyleC,   nullptr, g_hex_immediate_style_values, "Which style to use for printing hexadecimal disassembly values." },
    { "use-fast-stepping"                  , OptionValue::eTypeBoolean   , false, true,                       nullptr, nullptr, "Use a fast stepping algorithm based on running from branch to branch rather than instruction single-stepping." },
    { "load-script-from-symbol-file"       , OptionValue::eTypeEnum   ,    false, eLoadScriptFromSymFileWarn, nullptr, g_load_script_from_sym_file_values, "Allow LLDB to load scripting resources embedded in symbol files when available." },
    { "load-cwd-lldbinit"                  , OptionValue::eTypeEnum   ,    false, eLoadCWDlldbinitWarn,       nullptr, g_load_current_working_dir_lldbinit_values, "Allow LLDB to .lldbinit files from the current directory automatically." },
    { "memory-module-load-level"           , OptionValue::eTypeEnum   ,    false, eMemoryModuleLoadLevelComplete, nullptr, g_memory_module_load_level_values,
        "Loading modules from memory can be slow as reading the symbol tables and other data can take a long time depending on your connection to the debug target. "
        "This setting helps users control how much information gets loaded when loading modules from memory."
        "'complete' is the default value for this setting which will load all sections and symbols by reading them from memory (slowest, most accurate). "
        "'partial' will load sections and attempt to find function bounds without downloading the symbol table (faster, still accurate, missing symbol names). "
        "'minimal' is the fastest setting and will load section data with no symbols, but should rarely be used as stack frames in these memory regions will be inaccurate and not provide any context (fastest). " },
    { "display-expression-in-crashlogs"    , OptionValue::eTypeBoolean   , false, false,                      nullptr, nullptr, "Expressions that crash will show up in crash logs if the host system supports executable specific crash log strings and this setting is set to true." },
    { "trap-handler-names"                 , OptionValue::eTypeArray     , true,  OptionValue::eTypeString,   nullptr, nullptr, "A list of trap handler function names, e.g. a common Unix user process one is _sigtramp." },
    { "display-runtime-support-values"     , OptionValue::eTypeBoolean   , false, false,                      nullptr, nullptr, "If true, LLDB will show variables that are meant to support the operation of a language's runtime support." },
    { "non-stop-mode"                      , OptionValue::eTypeBoolean   , false, 0,                          nullptr, nullptr, "Disable lock-step debugging, instead control threads independently." },
    { nullptr                                 , OptionValue::eTypeInvalid   , false, 0                         , nullptr, nullptr, nullptr }
};

enum
{
    ePropertyDefaultArch,
    ePropertyMoveToNearestCode,
    ePropertyLanguage,
    ePropertyExprPrefix,
    ePropertyPreferDynamic,
    ePropertyEnableSynthetic,
    ePropertySkipPrologue,
    ePropertySourceMap,
    ePropertyExecutableSearchPaths,
    ePropertyDebugFileSearchPaths,
    ePropertyClangModuleSearchPaths,
    ePropertyAutoImportClangModules,
    ePropertyMaxChildrenCount,
    ePropertyMaxSummaryLength,
    ePropertyMaxMemReadSize,
    ePropertyBreakpointUseAvoidList,
    ePropertyArg0,
    ePropertyRunArgs,
    ePropertyEnvVars,
    ePropertyInheritEnv,
    ePropertyInputPath,
    ePropertyOutputPath,
    ePropertyErrorPath,
    ePropertyDetachOnError,
    ePropertyDisableASLR,
    ePropertyDisableSTDIO,
    ePropertyInlineStrategy,
    ePropertyDisassemblyFlavor,
    ePropertyUseHexImmediates,
    ePropertyHexImmediateStyle,
    ePropertyUseFastStepping,
    ePropertyLoadScriptFromSymbolFile,
    ePropertyLoadCWDlldbinitFile,
    ePropertyMemoryModuleLoadLevel,
    ePropertyDisplayExpressionsInCrashlogs,
    ePropertyTrapHandlerNames,
    ePropertyDisplayRuntimeSupportValues,
    ePropertyNonStopModeEnabled
};

class TargetOptionValueProperties : public OptionValueProperties
{
public:
    TargetOptionValueProperties (const ConstString &name) :
        OptionValueProperties (name),
        m_target(nullptr),
        m_got_host_env (false)
    {
    }

    // This constructor is used when creating TargetOptionValueProperties when it
    // is part of a new lldb_private::Target instance. It will copy all current
    // global property values as needed
    TargetOptionValueProperties (Target *target, const TargetPropertiesSP &target_properties_sp) :
        OptionValueProperties(*target_properties_sp->GetValueProperties()),
        m_target (target),
        m_got_host_env (false)
    {
    }

    const Property *
    GetPropertyAtIndex(const ExecutionContext *exe_ctx, bool will_modify, uint32_t idx) const override
    {
        // When getting the value for a key from the target options, we will always
        // try and grab the setting from the current target if there is one. Else we just
        // use the one from this instance.
        if (idx == ePropertyEnvVars)
            GetHostEnvironmentIfNeeded ();
            
        if (exe_ctx)
        {
            Target *target = exe_ctx->GetTargetPtr();
            if (target)
            {
                TargetOptionValueProperties *target_properties = static_cast<TargetOptionValueProperties *>(target->GetValueProperties().get());
                if (this != target_properties)
                    return target_properties->ProtectedGetPropertyAtIndex (idx);
            }
        }
        return ProtectedGetPropertyAtIndex (idx);
    }
    
    lldb::TargetSP
    GetTargetSP ()
    {
        return m_target->shared_from_this();
    }
    
protected:
    void
    GetHostEnvironmentIfNeeded () const
    {
        if (!m_got_host_env)
        {
            if (m_target)
            {
                m_got_host_env = true;
                const uint32_t idx = ePropertyInheritEnv;
                if (GetPropertyAtIndexAsBoolean(nullptr, idx, g_properties[idx].default_uint_value != 0))
                {
                    PlatformSP platform_sp (m_target->GetPlatform());
                    if (platform_sp)
                    {
                        StringList env;
                        if (platform_sp->GetEnvironment(env))
                        {
                            OptionValueDictionary *env_dict = GetPropertyAtIndexAsOptionValueDictionary(nullptr, ePropertyEnvVars);
                            if (env_dict)
                            {
                                const bool can_replace = false;
                                const size_t envc = env.GetSize();
                                for (size_t idx=0; idx<envc; idx++)
                                {
                                    const char *env_entry = env.GetStringAtIndex (idx);
                                    if (env_entry)
                                    {
                                        const char *equal_pos = ::strchr(env_entry, '=');
                                        ConstString key;
                                        // It is ok to have environment variables with no values
                                        const char *value = nullptr;
                                        if (equal_pos)
                                        {
                                            key.SetCStringWithLength(env_entry, equal_pos - env_entry);
                                            if (equal_pos[1])
                                                value = equal_pos + 1;
                                        }
                                        else
                                        {
                                            key.SetCString(env_entry);
                                        }
                                        // Don't allow existing keys to be replaced with ones we get from the platform environment
                                        env_dict->SetValueForKey(key, OptionValueSP(new OptionValueString(value)), can_replace);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    Target *m_target;
    mutable bool m_got_host_env;
};

//----------------------------------------------------------------------
// TargetProperties
//----------------------------------------------------------------------
TargetProperties::TargetProperties (Target *target) :
    Properties (),
    m_launch_info ()
{
    if (target)
    {
        m_collection_sp.reset (new TargetOptionValueProperties(target, Target::GetGlobalProperties()));

        // Set callbacks to update launch_info whenever "settins set" updated any of these properties
        m_collection_sp->SetValueChangedCallback(ePropertyArg0, TargetProperties::Arg0ValueChangedCallback, this);
        m_collection_sp->SetValueChangedCallback(ePropertyRunArgs, TargetProperties::RunArgsValueChangedCallback, this);
        m_collection_sp->SetValueChangedCallback(ePropertyEnvVars, TargetProperties::EnvVarsValueChangedCallback, this);
        m_collection_sp->SetValueChangedCallback(ePropertyInputPath, TargetProperties::InputPathValueChangedCallback, this);
        m_collection_sp->SetValueChangedCallback(ePropertyOutputPath, TargetProperties::OutputPathValueChangedCallback, this);
        m_collection_sp->SetValueChangedCallback(ePropertyErrorPath, TargetProperties::ErrorPathValueChangedCallback, this);
        m_collection_sp->SetValueChangedCallback(ePropertyDetachOnError, TargetProperties::DetachOnErrorValueChangedCallback, this);
        m_collection_sp->SetValueChangedCallback(ePropertyDisableASLR, TargetProperties::DisableASLRValueChangedCallback, this);
        m_collection_sp->SetValueChangedCallback(ePropertyDisableSTDIO, TargetProperties::DisableSTDIOValueChangedCallback, this);
    
        // Update m_launch_info once it was created
        Arg0ValueChangedCallback(this, nullptr);
        RunArgsValueChangedCallback(this, nullptr);
        //EnvVarsValueChangedCallback(this, nullptr); // FIXME: cause segfault in Target::GetPlatform()
        InputPathValueChangedCallback(this, nullptr);
        OutputPathValueChangedCallback(this, nullptr);
        ErrorPathValueChangedCallback(this, nullptr);
        DetachOnErrorValueChangedCallback(this, nullptr);
        DisableASLRValueChangedCallback(this, nullptr);
        DisableSTDIOValueChangedCallback(this, nullptr);
    }
    else
    {
        m_collection_sp.reset (new TargetOptionValueProperties(ConstString("target")));
        m_collection_sp->Initialize(g_properties);
        m_collection_sp->AppendProperty(ConstString("process"),
                                        ConstString("Settings specify to processes."),
                                        true,
                                        Process::GetGlobalProperties()->GetValueProperties());
    }
}

TargetProperties::~TargetProperties() = default;

ArchSpec
TargetProperties::GetDefaultArchitecture () const
{
    OptionValueArch *value = m_collection_sp->GetPropertyAtIndexAsOptionValueArch(nullptr, ePropertyDefaultArch);
    if (value)
        return value->GetCurrentValue();
    return ArchSpec();
}

void
TargetProperties::SetDefaultArchitecture (const ArchSpec& arch)
{
    OptionValueArch *value = m_collection_sp->GetPropertyAtIndexAsOptionValueArch(nullptr, ePropertyDefaultArch);
    if (value)
        return value->SetCurrentValue(arch, true);
}

bool
TargetProperties::GetMoveToNearestCode() const
{
    const uint32_t idx = ePropertyMoveToNearestCode;
    return m_collection_sp->GetPropertyAtIndexAsBoolean(nullptr, idx, g_properties[idx].default_uint_value != 0);
}

lldb::DynamicValueType
TargetProperties::GetPreferDynamicValue() const
{
    const uint32_t idx = ePropertyPreferDynamic;
    return (lldb::DynamicValueType)m_collection_sp->GetPropertyAtIndexAsEnumeration(nullptr, idx, g_properties[idx].default_uint_value);
}

bool
TargetProperties::SetPreferDynamicValue (lldb::DynamicValueType d)
{
    const uint32_t idx = ePropertyPreferDynamic;
    return m_collection_sp->SetPropertyAtIndexAsEnumeration(nullptr, idx, d);
}

bool
TargetProperties::GetDisableASLR () const
{
    const uint32_t idx = ePropertyDisableASLR;
    return m_collection_sp->GetPropertyAtIndexAsBoolean(nullptr, idx, g_properties[idx].default_uint_value != 0);
}

void
TargetProperties::SetDisableASLR (bool b)
{
    const uint32_t idx = ePropertyDisableASLR;
    m_collection_sp->SetPropertyAtIndexAsBoolean(nullptr, idx, b);
}

bool
TargetProperties::GetDetachOnError () const
{
    const uint32_t idx = ePropertyDetachOnError;
    return m_collection_sp->GetPropertyAtIndexAsBoolean(nullptr, idx, g_properties[idx].default_uint_value != 0);
}

void
TargetProperties::SetDetachOnError (bool b)
{
    const uint32_t idx = ePropertyDetachOnError;
    m_collection_sp->SetPropertyAtIndexAsBoolean(nullptr, idx, b);
}

bool
TargetProperties::GetDisableSTDIO () const
{
    const uint32_t idx = ePropertyDisableSTDIO;
    return m_collection_sp->GetPropertyAtIndexAsBoolean(nullptr, idx, g_properties[idx].default_uint_value != 0);
}

void
TargetProperties::SetDisableSTDIO (bool b)
{
    const uint32_t idx = ePropertyDisableSTDIO;
    m_collection_sp->SetPropertyAtIndexAsBoolean(nullptr, idx, b);
}

const char *
TargetProperties::GetDisassemblyFlavor () const
{
    const uint32_t idx = ePropertyDisassemblyFlavor;
    const char *return_value;
    
    x86DisassemblyFlavor flavor_value = (x86DisassemblyFlavor) m_collection_sp->GetPropertyAtIndexAsEnumeration(nullptr, idx, g_properties[idx].default_uint_value);
    return_value = g_x86_dis_flavor_value_types[flavor_value].string_value;
    return return_value;
}

InlineStrategy
TargetProperties::GetInlineStrategy () const
{
    const uint32_t idx = ePropertyInlineStrategy;
    return (InlineStrategy)m_collection_sp->GetPropertyAtIndexAsEnumeration(nullptr, idx, g_properties[idx].default_uint_value);
}

const char *
TargetProperties::GetArg0 () const
{
    const uint32_t idx = ePropertyArg0;
    return m_collection_sp->GetPropertyAtIndexAsString(nullptr, idx, nullptr);
}

void
TargetProperties::SetArg0 (const char *arg)
{
    const uint32_t idx = ePropertyArg0;
    m_collection_sp->SetPropertyAtIndexAsString(nullptr, idx, arg);
    m_launch_info.SetArg0(arg);
}

bool
TargetProperties::GetRunArguments (Args &args) const
{
    const uint32_t idx = ePropertyRunArgs;
    return m_collection_sp->GetPropertyAtIndexAsArgs(nullptr, idx, args);
}

void
TargetProperties::SetRunArguments (const Args &args)
{
    const uint32_t idx = ePropertyRunArgs;
    m_collection_sp->SetPropertyAtIndexFromArgs(nullptr, idx, args);
    m_launch_info.GetArguments() = args;
}

size_t
TargetProperties::GetEnvironmentAsArgs (Args &env) const
{
    const uint32_t idx = ePropertyEnvVars;
    return m_collection_sp->GetPropertyAtIndexAsArgs(nullptr, idx, env);
}

void
TargetProperties::SetEnvironmentFromArgs (const Args &env)
{
    const uint32_t idx = ePropertyEnvVars;
    m_collection_sp->SetPropertyAtIndexFromArgs(nullptr, idx, env);
    m_launch_info.GetEnvironmentEntries() = env;
}

bool
TargetProperties::GetSkipPrologue() const
{
    const uint32_t idx = ePropertySkipPrologue;
    return m_collection_sp->GetPropertyAtIndexAsBoolean(nullptr, idx, g_properties[idx].default_uint_value != 0);
}

PathMappingList &
TargetProperties::GetSourcePathMap () const
{
    const uint32_t idx = ePropertySourceMap;
    OptionValuePathMappings *option_value = m_collection_sp->GetPropertyAtIndexAsOptionValuePathMappings(nullptr, false, idx);
    assert(option_value);
    return option_value->GetCurrentValue();
}

FileSpecList &
TargetProperties::GetExecutableSearchPaths ()
{
    const uint32_t idx = ePropertyExecutableSearchPaths;
    OptionValueFileSpecList *option_value = m_collection_sp->GetPropertyAtIndexAsOptionValueFileSpecList(nullptr, false, idx);
    assert(option_value);
    return option_value->GetCurrentValue();
}

FileSpecList &
TargetProperties::GetDebugFileSearchPaths ()
{
    const uint32_t idx = ePropertyDebugFileSearchPaths;
    OptionValueFileSpecList *option_value = m_collection_sp->GetPropertyAtIndexAsOptionValueFileSpecList(nullptr, false, idx);
    assert(option_value);
    return option_value->GetCurrentValue();
}

FileSpecList &
TargetProperties::GetClangModuleSearchPaths ()
{
    const uint32_t idx = ePropertyClangModuleSearchPaths;
    OptionValueFileSpecList *option_value = m_collection_sp->GetPropertyAtIndexAsOptionValueFileSpecList(nullptr, false, idx);
    assert(option_value);
    return option_value->GetCurrentValue();
}

bool
TargetProperties::GetEnableAutoImportClangModules() const
{
    const uint32_t idx = ePropertyAutoImportClangModules;
    return m_collection_sp->GetPropertyAtIndexAsBoolean(nullptr, idx, g_properties[idx].default_uint_value != 0);
}

bool
TargetProperties::GetEnableSyntheticValue () const
{
    const uint32_t idx = ePropertyEnableSynthetic;
    return m_collection_sp->GetPropertyAtIndexAsBoolean(nullptr, idx, g_properties[idx].default_uint_value != 0);
}

uint32_t
TargetProperties::GetMaximumNumberOfChildrenToDisplay() const
{
    const uint32_t idx = ePropertyMaxChildrenCount;
    return m_collection_sp->GetPropertyAtIndexAsSInt64(nullptr, idx, g_properties[idx].default_uint_value);
}

uint32_t
TargetProperties::GetMaximumSizeOfStringSummary() const
{
    const uint32_t idx = ePropertyMaxSummaryLength;
    return m_collection_sp->GetPropertyAtIndexAsSInt64(nullptr, idx, g_properties[idx].default_uint_value);
}

uint32_t
TargetProperties::GetMaximumMemReadSize () const
{
    const uint32_t idx = ePropertyMaxMemReadSize;
    return m_collection_sp->GetPropertyAtIndexAsSInt64(nullptr, idx, g_properties[idx].default_uint_value);
}

FileSpec
TargetProperties::GetStandardInputPath () const
{
    const uint32_t idx = ePropertyInputPath;
    return m_collection_sp->GetPropertyAtIndexAsFileSpec(nullptr, idx);
}

void
TargetProperties::SetStandardInputPath (const char *p)
{
    const uint32_t idx = ePropertyInputPath;
    m_collection_sp->SetPropertyAtIndexAsString(nullptr, idx, p);
}

FileSpec
TargetProperties::GetStandardOutputPath () const
{
    const uint32_t idx = ePropertyOutputPath;
    return m_collection_sp->GetPropertyAtIndexAsFileSpec(nullptr, idx);
}

void
TargetProperties::SetStandardOutputPath (const char *p)
{
    const uint32_t idx = ePropertyOutputPath;
    m_collection_sp->SetPropertyAtIndexAsString(nullptr, idx, p);
}

FileSpec
TargetProperties::GetStandardErrorPath () const
{
    const uint32_t idx = ePropertyErrorPath;
    return m_collection_sp->GetPropertyAtIndexAsFileSpec(nullptr, idx);
}

LanguageType
TargetProperties::GetLanguage () const
{
    OptionValueLanguage *value = m_collection_sp->GetPropertyAtIndexAsOptionValueLanguage(nullptr, ePropertyLanguage);
    if (value)
        return value->GetCurrentValue();
    return LanguageType();
}

const char *
TargetProperties::GetExpressionPrefixContentsAsCString ()
{
    const uint32_t idx = ePropertyExprPrefix;
    OptionValueFileSpec *file = m_collection_sp->GetPropertyAtIndexAsOptionValueFileSpec(nullptr, false, idx);
    if (file)
    {
        const bool null_terminate = true;
        DataBufferSP data_sp(file->GetFileContents(null_terminate));
        if (data_sp)
            return (const char *) data_sp->GetBytes();
    }
    return nullptr;
}

void
TargetProperties::SetStandardErrorPath (const char *p)
{
    const uint32_t idx = ePropertyErrorPath;
    m_collection_sp->SetPropertyAtIndexAsString(nullptr, idx, p);
}

bool
TargetProperties::GetBreakpointsConsultPlatformAvoidList ()
{
    const uint32_t idx = ePropertyBreakpointUseAvoidList;
    return m_collection_sp->GetPropertyAtIndexAsBoolean(nullptr, idx, g_properties[idx].default_uint_value != 0);
}

bool
TargetProperties::GetUseHexImmediates () const
{
    const uint32_t idx = ePropertyUseHexImmediates;
    return m_collection_sp->GetPropertyAtIndexAsBoolean(nullptr, idx, g_properties[idx].default_uint_value != 0);
}

bool
TargetProperties::GetUseFastStepping () const
{
    const uint32_t idx = ePropertyUseFastStepping;
    return m_collection_sp->GetPropertyAtIndexAsBoolean(nullptr, idx, g_properties[idx].default_uint_value != 0);
}

bool
TargetProperties::GetDisplayExpressionsInCrashlogs () const
{
    const uint32_t idx = ePropertyDisplayExpressionsInCrashlogs;
    return m_collection_sp->GetPropertyAtIndexAsBoolean(nullptr, idx, g_properties[idx].default_uint_value != 0);
}

LoadScriptFromSymFile
TargetProperties::GetLoadScriptFromSymbolFile () const
{
    const uint32_t idx = ePropertyLoadScriptFromSymbolFile;
    return (LoadScriptFromSymFile)m_collection_sp->GetPropertyAtIndexAsEnumeration(nullptr, idx, g_properties[idx].default_uint_value);
}

LoadCWDlldbinitFile
TargetProperties::GetLoadCWDlldbinitFile () const
{
    const uint32_t idx = ePropertyLoadCWDlldbinitFile;
    return (LoadCWDlldbinitFile) m_collection_sp->GetPropertyAtIndexAsEnumeration(nullptr, idx, g_properties[idx].default_uint_value);
}

Disassembler::HexImmediateStyle
TargetProperties::GetHexImmediateStyle () const
{
    const uint32_t idx = ePropertyHexImmediateStyle;
    return (Disassembler::HexImmediateStyle)m_collection_sp->GetPropertyAtIndexAsEnumeration(nullptr, idx, g_properties[idx].default_uint_value);
}

MemoryModuleLoadLevel
TargetProperties::GetMemoryModuleLoadLevel() const
{
    const uint32_t idx = ePropertyMemoryModuleLoadLevel;
    return (MemoryModuleLoadLevel)m_collection_sp->GetPropertyAtIndexAsEnumeration(nullptr, idx, g_properties[idx].default_uint_value);
}

bool
TargetProperties::GetUserSpecifiedTrapHandlerNames (Args &args) const
{
    const uint32_t idx = ePropertyTrapHandlerNames;
    return m_collection_sp->GetPropertyAtIndexAsArgs(nullptr, idx, args);
}

void
TargetProperties::SetUserSpecifiedTrapHandlerNames (const Args &args)
{
    const uint32_t idx = ePropertyTrapHandlerNames;
    m_collection_sp->SetPropertyAtIndexFromArgs(nullptr, idx, args);
}

bool
TargetProperties::GetDisplayRuntimeSupportValues () const
{
    const uint32_t idx = ePropertyDisplayRuntimeSupportValues;
    return m_collection_sp->GetPropertyAtIndexAsBoolean(nullptr, idx, false);
}

void
TargetProperties::SetDisplayRuntimeSupportValues (bool b)
{
    const uint32_t idx = ePropertyDisplayRuntimeSupportValues;
    m_collection_sp->SetPropertyAtIndexAsBoolean(nullptr, idx, b);
}

bool
TargetProperties::GetNonStopModeEnabled () const
{
    const uint32_t idx = ePropertyNonStopModeEnabled;
    return m_collection_sp->GetPropertyAtIndexAsBoolean(nullptr, idx, false);
}

void
TargetProperties::SetNonStopModeEnabled (bool b)
{
    const uint32_t idx = ePropertyNonStopModeEnabled;
    m_collection_sp->SetPropertyAtIndexAsBoolean(nullptr, idx, b);
}

const ProcessLaunchInfo &
TargetProperties::GetProcessLaunchInfo ()
{
    m_launch_info.SetArg0(GetArg0()); // FIXME: Arg0 callback doesn't work
    return m_launch_info;
}

void
TargetProperties::SetProcessLaunchInfo(const ProcessLaunchInfo &launch_info)
{
    m_launch_info = launch_info;
    SetArg0(launch_info.GetArg0());
    SetRunArguments(launch_info.GetArguments());
    SetEnvironmentFromArgs(launch_info.GetEnvironmentEntries());
    const FileAction *input_file_action = launch_info.GetFileActionForFD(STDIN_FILENO);
    if (input_file_action)
    {
        const char *input_path = input_file_action->GetPath();
        if (input_path)
            SetStandardInputPath(input_path);
    }
    const FileAction *output_file_action = launch_info.GetFileActionForFD(STDOUT_FILENO);
    if (output_file_action)
    {
        const char *output_path = output_file_action->GetPath();
        if (output_path)
            SetStandardOutputPath(output_path);
    }
    const FileAction *error_file_action = launch_info.GetFileActionForFD(STDERR_FILENO);
    if (error_file_action)
    {
        const char *error_path = error_file_action->GetPath();
        if (error_path)
            SetStandardErrorPath(error_path);
    }
    SetDetachOnError(launch_info.GetFlags().Test(lldb::eLaunchFlagDetachOnError));
    SetDisableASLR(launch_info.GetFlags().Test(lldb::eLaunchFlagDisableASLR));
    SetDisableSTDIO(launch_info.GetFlags().Test(lldb::eLaunchFlagDisableSTDIO));
}

void
TargetProperties::Arg0ValueChangedCallback(void *target_property_ptr, OptionValue *)
{
    TargetProperties *this_ = reinterpret_cast<TargetProperties *>(target_property_ptr);
    this_->m_launch_info.SetArg0(this_->GetArg0());
}

void
TargetProperties::RunArgsValueChangedCallback(void *target_property_ptr, OptionValue *)
{
    TargetProperties *this_ = reinterpret_cast<TargetProperties *>(target_property_ptr);
    Args args;
    if (this_->GetRunArguments(args))
        this_->m_launch_info.GetArguments() = args;
}

void
TargetProperties::EnvVarsValueChangedCallback(void *target_property_ptr, OptionValue *)
{
    TargetProperties *this_ = reinterpret_cast<TargetProperties *>(target_property_ptr);
    Args args;
    if (this_->GetEnvironmentAsArgs(args))
        this_->m_launch_info.GetEnvironmentEntries() = args;
}

void
TargetProperties::InputPathValueChangedCallback(void *target_property_ptr, OptionValue *)
{
    TargetProperties *this_ = reinterpret_cast<TargetProperties *>(target_property_ptr);
    this_->m_launch_info.AppendOpenFileAction(STDIN_FILENO, this_->GetStandardInputPath(), true, false);
}

void
TargetProperties::OutputPathValueChangedCallback(void *target_property_ptr, OptionValue *)
{
    TargetProperties *this_ = reinterpret_cast<TargetProperties *>(target_property_ptr);
    this_->m_launch_info.AppendOpenFileAction(STDOUT_FILENO, this_->GetStandardOutputPath(), false, true);
}

void
TargetProperties::ErrorPathValueChangedCallback(void *target_property_ptr, OptionValue *)
{
    TargetProperties *this_ = reinterpret_cast<TargetProperties *>(target_property_ptr);
    this_->m_launch_info.AppendOpenFileAction(STDERR_FILENO, this_->GetStandardErrorPath(), false, true);
}

void
TargetProperties::DetachOnErrorValueChangedCallback(void *target_property_ptr, OptionValue *)
{
    TargetProperties *this_ = reinterpret_cast<TargetProperties *>(target_property_ptr);
    if (this_->GetDetachOnError())
        this_->m_launch_info.GetFlags().Set(lldb::eLaunchFlagDetachOnError);
    else
        this_->m_launch_info.GetFlags().Clear(lldb::eLaunchFlagDetachOnError);
}

void
TargetProperties::DisableASLRValueChangedCallback(void *target_property_ptr, OptionValue *)
{
    TargetProperties *this_ = reinterpret_cast<TargetProperties *>(target_property_ptr);
    if (this_->GetDisableASLR())
        this_->m_launch_info.GetFlags().Set(lldb::eLaunchFlagDisableASLR);
    else
        this_->m_launch_info.GetFlags().Clear(lldb::eLaunchFlagDisableASLR);
}

void
TargetProperties::DisableSTDIOValueChangedCallback(void *target_property_ptr, OptionValue *)
{
    TargetProperties *this_ = reinterpret_cast<TargetProperties *>(target_property_ptr);
    if (this_->GetDisableSTDIO())
        this_->m_launch_info.GetFlags().Set(lldb::eLaunchFlagDisableSTDIO);
    else
        this_->m_launch_info.GetFlags().Clear(lldb::eLaunchFlagDisableSTDIO);
}

//----------------------------------------------------------------------
// Target::TargetEventData
//----------------------------------------------------------------------

Target::TargetEventData::TargetEventData (const lldb::TargetSP &target_sp) :
    EventData (),
    m_target_sp (target_sp),
    m_module_list ()
{
}

Target::TargetEventData::TargetEventData (const lldb::TargetSP &target_sp, const ModuleList &module_list) :
    EventData (),
    m_target_sp (target_sp),
    m_module_list (module_list)
{
}

Target::TargetEventData::~TargetEventData() = default;

const ConstString &
Target::TargetEventData::GetFlavorString ()
{
    static ConstString g_flavor ("Target::TargetEventData");
    return g_flavor;
}

void
Target::TargetEventData::Dump (Stream *s) const
{
    for (size_t i = 0; i < m_module_list.GetSize(); ++i)
    {
        if (i != 0)
             *s << ", ";
        m_module_list.GetModuleAtIndex(i)->GetDescription(s, lldb::eDescriptionLevelBrief);
    }
}

const Target::TargetEventData *
Target::TargetEventData::GetEventDataFromEvent (const Event *event_ptr)
{
    if (event_ptr)
    {
        const EventData *event_data = event_ptr->GetData();
        if (event_data && event_data->GetFlavor() == TargetEventData::GetFlavorString())
            return static_cast <const TargetEventData *> (event_ptr->GetData());
    }
    return nullptr;
}

TargetSP
Target::TargetEventData::GetTargetFromEvent (const Event *event_ptr)
{
    TargetSP target_sp;
    const TargetEventData *event_data = GetEventDataFromEvent (event_ptr);
    if (event_data)
        target_sp = event_data->m_target_sp;
    return target_sp;
}

ModuleList
Target::TargetEventData::GetModuleListFromEvent (const Event *event_ptr)
{
    ModuleList module_list;
    const TargetEventData *event_data = GetEventDataFromEvent (event_ptr);
    if (event_data)
        module_list = event_data->m_module_list;
    return module_list;
}
