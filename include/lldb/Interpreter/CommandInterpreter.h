//===-- CommandInterpreter.h ------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_CommandInterpreter_h_
#define liblldb_CommandInterpreter_h_

// C Includes
// C++ Includes
// Other libraries and framework includes
// Project includes
#include "lldb/lldb-forward.h"
#include "lldb/lldb-private.h"
#include "lldb/Core/Broadcaster.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/IOHandler.h"
#include "lldb/Core/Log.h"
#include "lldb/Interpreter/CommandHistory.h"
#include "lldb/Interpreter/CommandObject.h"
#include "lldb/Interpreter/ScriptInterpreter.h"
#include "lldb/Core/Event.h"
#include "lldb/Interpreter/Args.h"
#include "lldb/Core/StringList.h"

namespace lldb_private {

class CommandInterpreterRunOptions
{
public:
    //------------------------------------------------------------------
    /// Construct a CommandInterpreterRunOptions object.
    /// This class is used to control all the instances where we run multiple commands, e.g.
    /// HandleCommands, HandleCommandsFromFile, RunCommandInterpreter.
    /// The meanings of the options in this object are:
    ///
    /// @param[in] stop_on_continue
    ///    If \b true execution will end on the first command that causes the process in the
    ///    execution context to continue.  If \false, we won't check the execution status.
    /// @param[in] stop_on_error 
    ///    If \b true execution will end on the first command that causes an error.
    /// @param[in] stop_on_crash
    ///    If \b true when a command causes the target to run, and the end of the run is a
    ///    signal or exception, stop executing the commands.
    /// @param[in] echo_commands
    ///    If \b true echo the command before executing it.  If \false, execute silently.
    /// @param[in] print_results
    ///    If \b true print the results of the command after executing it.  If \false, execute silently.
    /// @param[in] add_to_history
    ///    If \b true add the commands to the command history.  If \false, don't add them.
    //------------------------------------------------------------------
    CommandInterpreterRunOptions (LazyBool stop_on_continue,
                                  LazyBool stop_on_error,
                                  LazyBool stop_on_crash,
                                  LazyBool echo_commands,
                                  LazyBool print_results,
                                  LazyBool add_to_history) :
        m_stop_on_continue(stop_on_continue),
        m_stop_on_error(stop_on_error),
        m_stop_on_crash(stop_on_crash),
        m_echo_commands(echo_commands),
        m_print_results(print_results),
        m_add_to_history(add_to_history)
    {}

    CommandInterpreterRunOptions () :
        m_stop_on_continue(eLazyBoolCalculate),
        m_stop_on_error(eLazyBoolCalculate),
        m_stop_on_crash(eLazyBoolCalculate),
        m_echo_commands(eLazyBoolCalculate),
        m_print_results(eLazyBoolCalculate),
        m_add_to_history(eLazyBoolCalculate)
    {}

    void
    SetSilent (bool silent)
    {
        LazyBool value = silent ? eLazyBoolNo : eLazyBoolYes;

        m_echo_commands = value;
        m_print_results = value;
        m_add_to_history = value;
    }
    // These return the default behaviors if the behavior is not eLazyBoolCalculate.
    // But I've also left the ivars public since for different ways of running the
    // interpreter you might want to force different defaults...  In that case, just grab
    // the LazyBool ivars directly and do what you want with eLazyBoolCalculate.
    bool
    GetStopOnContinue () const
    {
        return DefaultToNo (m_stop_on_continue);
    }

    void
    SetStopOnContinue (bool stop_on_continue)
    {
        m_stop_on_continue = stop_on_continue ? eLazyBoolYes : eLazyBoolNo;
    }

    bool
    GetStopOnError () const
    {
        return DefaultToNo (m_stop_on_continue);
    }

    void
    SetStopOnError (bool stop_on_error)
    {
        m_stop_on_error = stop_on_error ? eLazyBoolYes : eLazyBoolNo;
    }

    bool
    GetStopOnCrash () const
    {
        return DefaultToNo (m_stop_on_crash);
    }

    void
    SetStopOnCrash (bool stop_on_crash)
    {
        m_stop_on_crash = stop_on_crash ? eLazyBoolYes : eLazyBoolNo;
    }

    bool
    GetEchoCommands () const
    {
        return DefaultToYes (m_echo_commands);
    }

    void
    SetEchoCommands (bool echo_commands)
    {
        m_echo_commands = echo_commands ? eLazyBoolYes : eLazyBoolNo;
    }

    bool
    GetPrintResults () const
    {
        return DefaultToYes (m_print_results);
    }

    void
    SetPrintResults (bool print_results)
    {
        m_print_results = print_results ? eLazyBoolYes : eLazyBoolNo;
    }

    bool
    GetAddToHistory () const
    {
        return DefaultToYes (m_add_to_history);
    }

    void
    SetAddToHistory (bool add_to_history)
    {
        m_add_to_history = add_to_history ? eLazyBoolYes : eLazyBoolNo;
    }

    LazyBool m_stop_on_continue;
    LazyBool m_stop_on_error;
    LazyBool m_stop_on_crash;
    LazyBool m_echo_commands;
    LazyBool m_print_results;
    LazyBool m_add_to_history;

    private:
    static bool
    DefaultToYes (LazyBool flag)
    {
        switch (flag)
        {
            case eLazyBoolNo:
                return false;
            default:
                return true;
        }
    }

    static bool
    DefaultToNo (LazyBool flag)
    {
        switch (flag)
        {
            case eLazyBoolYes:
                return true;
            default:
                return false;
        }
    }
};

class CommandInterpreter :
    public Broadcaster,
    public Properties,
    public IOHandlerDelegate
{
public:
    struct CommandAlias
    {
        lldb::CommandObjectSP m_underlying_command_sp = nullptr;
        OptionArgVectorSP m_option_args_sp = nullptr;
        
        CommandAlias (lldb::CommandObjectSP cmd_sp = nullptr,
                      OptionArgVectorSP args_sp = nullptr);
        
        void
        GetAliasHelp (StreamString &help_string);
        
        static bool
        ProcessAliasOptionsArgs (lldb::CommandObjectSP &cmd_obj_sp,
                                 const char *options_args,
                                 OptionArgVectorSP &option_arg_vector_sp);
        
        bool
        IsValid ()
        {
            return m_underlying_command_sp && m_option_args_sp;
        }
        
        explicit operator bool ()
        {
            return IsValid();
        }
    };
    
    typedef std::map<std::string, CommandAlias> CommandAliasMap;
    
    enum
    {
        eBroadcastBitThreadShouldExit       = (1 << 0),
        eBroadcastBitResetPrompt            = (1 << 1),
        eBroadcastBitQuitCommandReceived    = (1 << 2),   // User entered quit
        eBroadcastBitAsynchronousOutputData = (1 << 3),
        eBroadcastBitAsynchronousErrorData  = (1 << 4)
    };
    
    enum ChildrenTruncatedWarningStatus // tristate boolean to manage children truncation warning
    {
        eNoTruncation = 0, // never truncated
        eUnwarnedTruncation = 1, // truncated but did not notify
        eWarnedTruncation = 2 // truncated and notified
    };
    
    enum CommandTypes
    {
        eCommandTypesBuiltin = 0x0001,  // native commands such as "frame"
        eCommandTypesUserDef = 0x0002,  // scripted commands
        eCommandTypesAliases = 0x0004,  // aliases such as "po"
        eCommandTypesHidden  = 0x0008,  // commands prefixed with an underscore
        eCommandTypesAllThem = 0xFFFF   // all commands
    };

    CommandInterpreter(Debugger &debugger,
                       lldb::ScriptLanguage script_language,
                       bool synchronous_execution);

    ~CommandInterpreter() override;

    // These two functions fill out the Broadcaster interface:
    
    static ConstString &GetStaticBroadcasterClass ();

    ConstString &GetBroadcasterClass() const override
    {
        return GetStaticBroadcasterClass();
    }

    void
    SourceInitFile (bool in_cwd, 
                    CommandReturnObject &result);

    bool
    AddCommand (const char *name, 
                const lldb::CommandObjectSP &cmd_sp,
                bool can_replace);
    
    bool
    AddUserCommand (std::string name, 
                    const lldb::CommandObjectSP &cmd_sp,
                    bool can_replace);
    
    lldb::CommandObjectSP
    GetCommandSPExact (const char *cmd, 
                       bool include_aliases);

    CommandObject *
    GetCommandObjectExact (const char *cmd_cstr, 
                           bool include_aliases);

    CommandObject *
    GetCommandObject(const char *cmd,
                     StringList *matches = nullptr);

    bool
    CommandExists (const char *cmd);

    bool
    AliasExists (const char *cmd);

    bool
    UserCommandExists (const char *cmd);

    bool
    AddAlias (const char *alias_name,
              lldb::CommandObjectSP& command_obj_sp,
              const char *args_string = nullptr);
    
    // Remove a command if it is removable (python or regex command)
    bool
    RemoveCommand (const char *cmd);

    bool
    RemoveAlias (const char *alias_name);
    
    bool
    GetAliasFullName (const char *cmd, std::string &full_name);

    bool
    RemoveUser (const char *alias_name);
    
    void
    RemoveAllUser ()
    {
        m_user_dict.clear();
    }

    CommandAlias
    GetAlias (const char *alias_name);

    CommandObject *
    BuildAliasResult (const char *alias_name, 
                      std::string &raw_input_string, 
                      std::string &alias_result, 
                      CommandReturnObject &result);

    bool
    HandleCommand(const char *command_line,
                  LazyBool add_to_history,
                  CommandReturnObject &result,
                  ExecutionContext *override_context = nullptr,
                  bool repeat_on_empty_command = true,
                  bool no_context_switching = false);
    
    //------------------------------------------------------------------
    /// Execute a list of commands in sequence.
    ///
    /// @param[in] commands
    ///    The list of commands to execute.
    /// @param[in,out] context
    ///    The execution context in which to run the commands. Can be nullptr in which case the default
    ///    context will be used.
    /// @param[in] options
    ///    This object holds the options used to control when to stop, whether to execute commands,
    ///    etc.
    /// @param[out] result
    ///    This is marked as succeeding with no output if all commands execute safely,
    ///    and failed with some explanation if we aborted executing the commands at some point.
    //------------------------------------------------------------------
    void
    HandleCommands (const StringList &commands, 
                    ExecutionContext *context,
                    CommandInterpreterRunOptions &options,
                    CommandReturnObject &result);

    //------------------------------------------------------------------
    /// Execute a list of commands from a file.
    ///
    /// @param[in] file
    ///    The file from which to read in commands.
    /// @param[in,out] context
    ///    The execution context in which to run the commands. Can be nullptr in which case the default
    ///    context will be used.
    /// @param[in] options
    ///    This object holds the options used to control when to stop, whether to execute commands,
    ///    etc.
    /// @param[out] result
    ///    This is marked as succeeding with no output if all commands execute safely,
    ///    and failed with some explanation if we aborted executing the commands at some point.
    //------------------------------------------------------------------
    void
    HandleCommandsFromFile (FileSpec &file, 
                            ExecutionContext *context,
                            CommandInterpreterRunOptions &options,
                            CommandReturnObject &result);

    CommandObject *
    GetCommandObjectForCommand (std::string &command_line);

    // This handles command line completion.  You are given a pointer to the command string buffer, to the current cursor,
    // and to the end of the string (in case it is not NULL terminated).
    // You also passed in an StringList object to fill with the returns.
    // The first element of the array will be filled with the string that you would need to insert at
    // the cursor point to complete the cursor point to the longest common matching prefix.
    // If you want to limit the number of elements returned, set max_return_elements to the number of elements
    // you want returned.  Otherwise set max_return_elements to -1.
    // If you want to start some way into the match list, then set match_start_point to the desired start
    // point.
    // Returns:
    // -1 if the completion character should be inserted
    // -2 if the entire command line should be deleted and replaced with matches.GetStringAtIndex(0)
    // INT_MAX if the number of matches is > max_return_elements, but it is expensive to compute.
    // Otherwise, returns the number of matches.
    //
    // FIXME: Only max_return_elements == -1 is supported at present.
    int
    HandleCompletion (const char *current_line,
                      const char *cursor,
                      const char *last_char,
                      int match_start_point,
                      int max_return_elements,
                      StringList &matches);

    // This version just returns matches, and doesn't compute the substring.  It is here so the
    // Help command can call it for the first argument.
    // word_complete tells whether the completions are considered a "complete" response (so the
    // completer should complete the quote & put a space after the word.
    int
    HandleCompletionMatches (Args &input,
                             int &cursor_index,
                             int &cursor_char_position,
                             int match_start_point,
                             int max_return_elements,
                             bool &word_complete,
                             StringList &matches);

    int
    GetCommandNamesMatchingPartialString (const char *cmd_cstr, 
                                          bool include_aliases, 
                                          StringList &matches);

    void
    GetHelp (CommandReturnObject &result,
             uint32_t types = eCommandTypesAllThem);

    void
    GetAliasHelp (const char *alias_name, 
                  StreamString &help_string);

    void
    OutputFormattedHelpText (Stream &strm,
                             const char *prefix,
                             const char *help_text);

    void
    OutputFormattedHelpText (Stream &stream,
                             const char *command_word,
                             const char *separator,
                             const char *help_text,
                             size_t max_word_len);
    
    // this mimics OutputFormattedHelpText but it does perform a much simpler
    // formatting, basically ensuring line alignment. This is only good if you have
    // some complicated layout for your help text and want as little help as reasonable
    // in properly displaying it. Most of the times, you simply want to type some text
    // and have it printed in a reasonable way on screen. If so, use OutputFormattedHelpText 
    void
    OutputHelpText (Stream &stream,
                    const char *command_word,
                    const char *separator,
                    const char *help_text,
                    uint32_t max_word_len);

    Debugger &
    GetDebugger ()
    {
        return m_debugger;
    }
    
    ExecutionContext
    GetExecutionContext()
    {
        const bool thread_and_frame_only_if_stopped = true;
        return m_exe_ctx_ref.Lock(thread_and_frame_only_if_stopped);
    }
    
    void
    UpdateExecutionContext (ExecutionContext *override_context);

    lldb::PlatformSP
    GetPlatform (bool prefer_target_platform);

    const char *
    ProcessEmbeddedScriptCommands (const char *arg);

    void
    UpdatePrompt (const char *);
    
    bool
    Confirm (const char *message,
             bool default_answer);
    
    void
    LoadCommandDictionary ();

    void
    Initialize ();
    
    void
    Clear ();

    void
    SetScriptLanguage (lldb::ScriptLanguage lang);

    bool
    HasCommands ();

    bool
    HasAliases ();

    bool
    HasUserCommands ();

    bool
    HasAliasOptions ();

    void
    BuildAliasCommandArgs (CommandObject *alias_cmd_obj, 
                           const char *alias_name, 
                           Args &cmd_args, 
                           std::string &raw_input_string, 
                           CommandReturnObject &result);

    int
    GetOptionArgumentPosition (const char *in_string);

    ScriptInterpreter *
    GetScriptInterpreter(bool can_create = true);

    void
    SetScriptInterpreter();

    void
    SkipLLDBInitFiles (bool skip_lldbinit_files)
    {
        m_skip_lldbinit_files = skip_lldbinit_files;
    }

    void
    SkipAppInitFiles (bool skip_app_init_files)
    {
        m_skip_app_init_files = m_skip_lldbinit_files;
    }

    bool
    GetSynchronous ();
    
    void
    FindCommandsForApropos (const char *word, 
                            StringList &commands_found, 
                            StringList &commands_help,
                            bool search_builtin_commands,
                            bool search_user_commands);
                           
    bool
    GetBatchCommandMode () { return m_batch_command_mode; }
    
    bool
    SetBatchCommandMode (bool value) {
        const bool old_value = m_batch_command_mode;
        m_batch_command_mode = value;
        return old_value;
    }
    
    void
    ChildrenTruncated ()
    {
        if (m_truncation_warning == eNoTruncation)
            m_truncation_warning = eUnwarnedTruncation;
    }
    
    bool
    TruncationWarningNecessary ()
    {
        return (m_truncation_warning == eUnwarnedTruncation);
    }
    
    void
    TruncationWarningGiven ()
    {
        m_truncation_warning = eWarnedTruncation;
    }
    
    const char *
    TruncationWarningText ()
    {
        return "*** Some of your variables have more members than the debugger will show by default. To show all of them, you can either use the --show-all-children option to %s or raise the limit by changing the target.max-children-count setting.\n";
    }
    
    const CommandHistory&
    GetCommandHistory () const
    {
        return m_command_history;
    }
    
    CommandHistory&
    GetCommandHistory ()
    {
        return m_command_history;
    }
    
    bool
    IsActive ();

    void
    RunCommandInterpreter (bool auto_handle_events,
                           bool spawn_thread,
                           CommandInterpreterRunOptions &options);
    void
    GetLLDBCommandsFromIOHandler (const char *prompt,
                                  IOHandlerDelegate &delegate,
                                  bool asynchronously,
                                  void *baton);

    void
    GetPythonCommandsFromIOHandler (const char *prompt,
                                    IOHandlerDelegate &delegate,
                                    bool asynchronously,
                                    void *baton);

    const char *
    GetCommandPrefix ();

    //------------------------------------------------------------------
    // Properties
    //------------------------------------------------------------------
    bool
    GetExpandRegexAliases () const;
    
    bool
    GetPromptOnQuit () const;

    void
    SetPromptOnQuit (bool b);

    void
    ResolveCommand(const char *command_line, CommandReturnObject &result);

    bool
    GetStopCmdSourceOnError () const;

    uint32_t
    GetNumErrors() const
    {
        return m_num_errors;
    }

    bool
    GetQuitRequested () const
    {
        return m_quit_requested;
    }

    lldb::IOHandlerSP
    GetIOHandler(bool force_create = false, CommandInterpreterRunOptions *options = nullptr);

    bool
    GetStoppedForCrash () const
    {
        return m_stopped_for_crash;
    }
    
    bool
    GetSpaceReplPrompts () const;
    
protected:
    friend class Debugger;

    //------------------------------------------------------------------
    // IOHandlerDelegate functions
    //------------------------------------------------------------------
    void
    IOHandlerInputComplete(IOHandler &io_handler,
                           std::string &line) override;

    ConstString
    IOHandlerGetControlSequence(char ch) override
    {
        if (ch == 'd')
            return ConstString("quit\n");
        return ConstString();
    }
    
    bool
    IOHandlerInterrupt(IOHandler &io_handler) override;

    size_t
    GetProcessOutput ();

    void
    SetSynchronous (bool value);

    lldb::CommandObjectSP
    GetCommandSP(const char *cmd, bool include_aliases = true, bool exact = true, StringList *matches = nullptr);
    
private:
    Error
    PreprocessCommand (std::string &command);

    // Completely resolves aliases and abbreviations, returning a pointer to the
    // final command object and updating command_line to the fully substituted
    // and translated command.
    CommandObject *
    ResolveCommandImpl(std::string &command_line, CommandReturnObject &result);

    Debugger &m_debugger;                       // The debugger session that this interpreter is associated with
    ExecutionContextRef m_exe_ctx_ref;          // The current execution context to use when handling commands
    bool m_synchronous_execution;
    bool m_skip_lldbinit_files;
    bool m_skip_app_init_files;
    CommandObject::CommandMap m_command_dict;   // Stores basic built-in commands (they cannot be deleted, removed or overwritten).
    CommandAliasMap m_alias_dict;               // Stores user aliases/abbreviations for commands
    CommandObject::CommandMap m_user_dict;      // Stores user-defined commands
    CommandHistory m_command_history;
    std::string m_repeat_command;               // Stores the command that will be executed for an empty command string.
    lldb::ScriptInterpreterSP m_script_interpreter_sp;
    lldb::IOHandlerSP m_command_io_handler_sp;
    char m_comment_char;
    bool m_batch_command_mode;
    ChildrenTruncatedWarningStatus m_truncation_warning;    // Whether we truncated children and whether the user has been told
    uint32_t m_command_source_depth;
    std::vector<uint32_t> m_command_source_flags;
    uint32_t m_num_errors;
    bool m_quit_requested;
    bool m_stopped_for_crash;
};

} // namespace lldb_private

#endif // liblldb_CommandInterpreter_h_
