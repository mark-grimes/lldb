//===-- CommandObjectSource.cpp ---------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// C Includes
// C++ Includes
// Other libraries and framework includes
#include "llvm/ADT/StringRef.h"

// Project includes
#include "CommandObjectCommands.h"
#include "CommandObjectHelp.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/IOHandler.h"
#include "lldb/Core/StringList.h"
#include "lldb/Interpreter/Args.h"
#include "lldb/Interpreter/CommandHistory.h"
#include "lldb/Interpreter/CommandInterpreter.h"
#include "lldb/Interpreter/CommandObjectRegexCommand.h"
#include "lldb/Interpreter/CommandReturnObject.h"
#include "lldb/Interpreter/OptionValueBoolean.h"
#include "lldb/Interpreter/OptionValueUInt64.h"
#include "lldb/Interpreter/Options.h"
#include "lldb/Interpreter/ScriptInterpreter.h"

using namespace lldb;
using namespace lldb_private;

//-------------------------------------------------------------------------
// CommandObjectCommandsSource
//-------------------------------------------------------------------------

class CommandObjectCommandsHistory : public CommandObjectParsed
{
public:
    CommandObjectCommandsHistory(CommandInterpreter &interpreter) :
        CommandObjectParsed(interpreter,
                            "command history",
                            "Dump the history of commands in this session.",
                            nullptr),
        m_options (interpreter)
    {
    }

    ~CommandObjectCommandsHistory() override = default;

    Options *
    GetOptions () override
    {
        return &m_options;
    }

protected:
    class CommandOptions : public Options
    {
    public:
        CommandOptions (CommandInterpreter &interpreter) :
            Options (interpreter),
            m_start_idx(0),
            m_stop_idx(0),
            m_count(0),
            m_clear(false)
        {
        }

        ~CommandOptions() override = default;

        Error
        SetOptionValue (uint32_t option_idx, const char *option_arg) override
        {
            Error error;
            const int short_option = m_getopt_table[option_idx].val;
            
            switch (short_option)
            {
                case 'c':
                    error = m_count.SetValueFromString(option_arg,eVarSetOperationAssign);
                    break;
                case 's':
                    if (option_arg && strcmp("end", option_arg) == 0)
                    {
                        m_start_idx.SetCurrentValue(UINT64_MAX);
                        m_start_idx.SetOptionWasSet();
                    }
                    else
                        error = m_start_idx.SetValueFromString(option_arg,eVarSetOperationAssign);
                    break;
                case 'e':
                    error = m_stop_idx.SetValueFromString(option_arg,eVarSetOperationAssign);
                    break;
                case 'C':
                    m_clear.SetCurrentValue(true);
                    m_clear.SetOptionWasSet();
                    break;
                default:
                    error.SetErrorStringWithFormat ("unrecognized option '%c'", short_option);
                    break;
            }
            
            return error;
        }

        void
        OptionParsingStarting () override
        {
            m_start_idx.Clear();
            m_stop_idx.Clear();
            m_count.Clear();
            m_clear.Clear();
        }

        const OptionDefinition*
        GetDefinitions () override
        {
            return g_option_table;
        }

        // Options table: Required for subclasses of Options.

        static OptionDefinition g_option_table[];

        // Instance variables to hold the values for command options.

        OptionValueUInt64 m_start_idx;
        OptionValueUInt64 m_stop_idx;
        OptionValueUInt64 m_count;
        OptionValueBoolean m_clear;
    };
    
    bool
    DoExecute (Args& command, CommandReturnObject &result) override
    {
        if (m_options.m_clear.GetCurrentValue() && m_options.m_clear.OptionWasSet())
        {
            m_interpreter.GetCommandHistory().Clear();
            result.SetStatus(lldb::eReturnStatusSuccessFinishNoResult);
        }
        else
        {
            if (m_options.m_start_idx.OptionWasSet() && m_options.m_stop_idx.OptionWasSet() && m_options.m_count.OptionWasSet())
            {
                result.AppendError("--count, --start-index and --end-index cannot be all specified in the same invocation");
                result.SetStatus(lldb::eReturnStatusFailed);
            }
            else
            {
                std::pair<bool,uint64_t> start_idx(m_options.m_start_idx.OptionWasSet(),m_options.m_start_idx.GetCurrentValue());
                std::pair<bool,uint64_t> stop_idx(m_options.m_stop_idx.OptionWasSet(),m_options.m_stop_idx.GetCurrentValue());
                std::pair<bool,uint64_t> count(m_options.m_count.OptionWasSet(),m_options.m_count.GetCurrentValue());
                
                const CommandHistory& history(m_interpreter.GetCommandHistory());
                                              
                if (start_idx.first && start_idx.second == UINT64_MAX)
                {
                    if (count.first)
                    {
                        start_idx.second = history.GetSize() - count.second;
                        stop_idx.second = history.GetSize() - 1;
                    }
                    else if (stop_idx.first)
                    {
                        start_idx.second = stop_idx.second;
                        stop_idx.second = history.GetSize() - 1;
                    }
                    else
                    {
                        start_idx.second = 0;
                        stop_idx.second = history.GetSize() - 1;
                    }
                }
                else
                {
                    if (!start_idx.first && !stop_idx.first && !count.first)
                    {
                        start_idx.second = 0;
                        stop_idx.second = history.GetSize() - 1;
                    }
                    else if (start_idx.first)
                    {
                        if (count.first)
                        {
                            stop_idx.second = start_idx.second + count.second - 1;
                        }
                        else if (!stop_idx.first)
                        {
                            stop_idx.second = history.GetSize() - 1;
                        }
                    }
                    else if (stop_idx.first)
                    {
                        if (count.first)
                        {
                            if (stop_idx.second >= count.second)
                                start_idx.second = stop_idx.second - count.second + 1;
                            else
                                start_idx.second = 0;
                        }
                    }
                    else /* if (count.first) */
                    {
                        start_idx.second = 0;
                        stop_idx.second = count.second - 1;
                    }
                }
                history.Dump(result.GetOutputStream(), start_idx.second, stop_idx.second);
            }
        }
        return result.Succeeded();

    }

    CommandOptions m_options;
};

OptionDefinition
CommandObjectCommandsHistory::CommandOptions::g_option_table[] =
{
{ LLDB_OPT_SET_1, false, "count", 'c', OptionParser::eRequiredArgument, nullptr, nullptr, 0, eArgTypeUnsignedInteger,        "How many history commands to print."},
{ LLDB_OPT_SET_1, false, "start-index", 's', OptionParser::eRequiredArgument, nullptr, nullptr, 0, eArgTypeUnsignedInteger,  "Index at which to start printing history commands (or end to mean tail mode)."},
{ LLDB_OPT_SET_1, false, "end-index", 'e', OptionParser::eRequiredArgument, nullptr, nullptr, 0, eArgTypeUnsignedInteger,    "Index at which to stop printing history commands."},
{ LLDB_OPT_SET_2, false, "clear", 'C', OptionParser::eNoArgument, nullptr, nullptr, 0, eArgTypeBoolean,    "Clears the current command history."},
{ 0, false, nullptr, 0, 0, nullptr, nullptr, 0, eArgTypeNone, nullptr }
};

//-------------------------------------------------------------------------
// CommandObjectCommandsSource
//-------------------------------------------------------------------------

class CommandObjectCommandsSource : public CommandObjectParsed
{
public:
    CommandObjectCommandsSource(CommandInterpreter &interpreter) :
        CommandObjectParsed(interpreter,
                            "command source",
                            "Read in debugger commands from the file <filename> and execute them.",
                            nullptr),
        m_options (interpreter)
    {
        CommandArgumentEntry arg;
        CommandArgumentData file_arg;
        
        // Define the first (and only) variant of this arg.
        file_arg.arg_type = eArgTypeFilename;
        file_arg.arg_repetition = eArgRepeatPlain;
        
        // There is only one variant this argument could be; put it into the argument entry.
        arg.push_back (file_arg);
        
        // Push the data for the first argument into the m_arguments vector.
        m_arguments.push_back (arg);
    }

    ~CommandObjectCommandsSource() override = default;

    const char*
    GetRepeatCommand (Args &current_command_args, uint32_t index) override
    {
        return "";
    }
    
    int
    HandleArgumentCompletion (Args &input,
                              int &cursor_index,
                              int &cursor_char_position,
                              OptionElementVector &opt_element_vector,
                              int match_start_point,
                              int max_return_elements,
                              bool &word_complete,
                              StringList &matches) override
    {
        std::string completion_str (input.GetArgumentAtIndex(cursor_index));
        completion_str.erase (cursor_char_position);
        
        CommandCompletions::InvokeCommonCompletionCallbacks(m_interpreter,
                                                            CommandCompletions::eDiskFileCompletion,
                                                            completion_str.c_str(),
                                                            match_start_point,
                                                            max_return_elements,
                                                            nullptr,
                                                            word_complete,
                                                            matches);
        return matches.GetSize();
    }

    Options *
    GetOptions () override
    {
        return &m_options;
    }

protected:
    class CommandOptions : public Options
    {
    public:
        CommandOptions (CommandInterpreter &interpreter) :
            Options (interpreter),
            m_stop_on_error (true),
            m_silent_run (false),
            m_stop_on_continue (true)
        {
        }

        ~CommandOptions() override = default;

        Error
        SetOptionValue (uint32_t option_idx, const char *option_arg) override
        {
            Error error;
            const int short_option = m_getopt_table[option_idx].val;
            
            switch (short_option)
            {
                case 'e':
                    error = m_stop_on_error.SetValueFromString(option_arg);
                    break;

                case 'c':
                    error = m_stop_on_continue.SetValueFromString(option_arg);
                    break;

                case 's':
                    error = m_silent_run.SetValueFromString(option_arg);
                    break;

                default:
                    error.SetErrorStringWithFormat ("unrecognized option '%c'", short_option);
                    break;
            }
            
            return error;
        }

        void
        OptionParsingStarting () override
        {
            m_stop_on_error.Clear();
            m_silent_run.Clear();
            m_stop_on_continue.Clear();
        }

        const OptionDefinition*
        GetDefinitions () override
        {
            return g_option_table;
        }

        // Options table: Required for subclasses of Options.

        static OptionDefinition g_option_table[];

        // Instance variables to hold the values for command options.

        OptionValueBoolean m_stop_on_error;
        OptionValueBoolean m_silent_run;
        OptionValueBoolean m_stop_on_continue;
    };
    
    bool
    DoExecute(Args& command, CommandReturnObject &result) override
    {
        const size_t argc = command.GetArgumentCount();
        if (argc == 1)
        {
            const char *filename = command.GetArgumentAtIndex(0);

            FileSpec cmd_file (filename, true);
            ExecutionContext *exe_ctx = nullptr;  // Just use the default context.
            
            // If any options were set, then use them
            if (m_options.m_stop_on_error.OptionWasSet()    ||
                m_options.m_silent_run.OptionWasSet()       ||
                m_options.m_stop_on_continue.OptionWasSet())
            {
                // Use user set settings
                CommandInterpreterRunOptions options;
                options.SetStopOnContinue(m_options.m_stop_on_continue.GetCurrentValue());
                options.SetStopOnError (m_options.m_stop_on_error.GetCurrentValue());
                options.SetEchoCommands (!m_options.m_silent_run.GetCurrentValue());
                options.SetPrintResults (!m_options.m_silent_run.GetCurrentValue());

                m_interpreter.HandleCommandsFromFile (cmd_file,
                                                      exe_ctx,
                                                      options,
                                                      result);
            }
            else
            {
                // No options were set, inherit any settings from nested "command source" commands,
                // or set to sane default settings...
                CommandInterpreterRunOptions options;
                m_interpreter.HandleCommandsFromFile (cmd_file,
                                                      exe_ctx,
                                                      options,
                                                      result);
            }
        }
        else
        {
            result.AppendErrorWithFormat("'%s' takes exactly one executable filename argument.\n", GetCommandName());
            result.SetStatus (eReturnStatusFailed);
        }
        return result.Succeeded();
    }

    CommandOptions m_options;    
};

OptionDefinition
CommandObjectCommandsSource::CommandOptions::g_option_table[] =
{
{ LLDB_OPT_SET_ALL, false, "stop-on-error", 'e', OptionParser::eRequiredArgument, nullptr, nullptr, 0, eArgTypeBoolean,    "If true, stop executing commands on error."},
{ LLDB_OPT_SET_ALL, false, "stop-on-continue", 'c', OptionParser::eRequiredArgument, nullptr, nullptr, 0, eArgTypeBoolean, "If true, stop executing commands on continue."},
{ LLDB_OPT_SET_ALL, false, "silent-run", 's', OptionParser::eRequiredArgument, nullptr, nullptr, 0, eArgTypeBoolean, "If true don't echo commands while executing."},
{ 0, false, nullptr, 0, 0, nullptr, nullptr, 0, eArgTypeNone, nullptr }
};

#pragma mark CommandObjectCommandsAlias
//-------------------------------------------------------------------------
// CommandObjectCommandsAlias
//-------------------------------------------------------------------------

static const char *g_python_command_instructions =   "Enter your Python command(s). Type 'DONE' to end.\n"
                                                     "You must define a Python function with this signature:\n"
                                                     "def my_command_impl(debugger, args, result, internal_dict):\n";

class CommandObjectCommandsAlias : public CommandObjectRaw
{
public:
    CommandObjectCommandsAlias (CommandInterpreter &interpreter) :
        CommandObjectRaw(interpreter,
                         "command alias",
                         "Allow users to define their own debugger command abbreviations.",
                         nullptr)
    {
        SetHelpLong(
"'alias' allows the user to create a short-cut or abbreviation for long \
commands, multi-word commands, and commands that take particular options.  \
Below are some simple examples of how one might use the 'alias' command:" R"(

(lldb) command alias sc script

    Creates the abbreviation 'sc' for the 'script' command.

(lldb) command alias bp breakpoint

)" "    Creates the abbreviation 'bp' for the 'breakpoint' command.  Since \
breakpoint commands are two-word commands, the user would still need to \
enter the second word after 'bp', e.g. 'bp enable' or 'bp delete'." R"(

(lldb) command alias bpl breakpoint list

    Creates the abbreviation 'bpl' for the two-word command 'breakpoint list'.

)" "An alias can include some options for the command, with the values either \
filled in at the time the alias is created, or specified as positional \
arguments, to be filled in when the alias is invoked.  The following example \
shows how to create aliases with options:" R"(

(lldb) command alias bfl breakpoint set -f %1 -l %2

)" "    Creates the abbreviation 'bfl' (for break-file-line), with the -f and -l \
options already part of the alias.  So if the user wants to set a breakpoint \
by file and line without explicitly having to use the -f and -l options, the \
user can now use 'bfl' instead.  The '%1' and '%2' are positional placeholders \
for the actual arguments that will be passed when the alias command is used.  \
The number in the placeholder refers to the position/order the actual value \
occupies when the alias is used.  All the occurrences of '%1' in the alias \
will be replaced with the first argument, all the occurrences of '%2' in the \
alias will be replaced with the second argument, and so on.  This also allows \
actual arguments to be used multiple times within an alias (see 'process \
launch' example below)." R"(

)" "Note: the positional arguments must substitute as whole words in the resultant \
command, so you can't at present do something like this to append the file extension \
\".cpp\":" R"(

(lldb) command alias bcppfl breakpoint set -f %1.cpp -l %2

)" "For more complex aliasing, use the \"command regex\" command instead.  In the \
'bfl' case above, the actual file value will be filled in with the first argument \
following 'bfl' and the actual line number value will be filled in with the second \
argument.  The user would use this alias as follows:" R"(

(lldb) command alias bfl breakpoint set -f %1 -l %2
(lldb) bfl my-file.c 137

This would be the same as if the user had entered 'breakpoint set -f my-file.c -l 137'.

Another example:

(lldb) command alias pltty process launch -s -o %1 -e %1
(lldb) pltty /dev/tty0

    Interpreted as 'process launch -s -o /dev/tty0 -e /dev/tty0'

)" "If the user always wanted to pass the same value to a particular option, the \
alias could be defined with that value directly in the alias as a constant, \
rather than using a positional placeholder:" R"(

(lldb) command alias bl3 breakpoint set -f %1 -l 3

    Always sets a breakpoint on line 3 of whatever file is indicated.)"
        );

        CommandArgumentEntry arg1;
        CommandArgumentEntry arg2;
        CommandArgumentEntry arg3;
        CommandArgumentData alias_arg;
        CommandArgumentData cmd_arg;
        CommandArgumentData options_arg;
        
        // Define the first (and only) variant of this arg.
        alias_arg.arg_type = eArgTypeAliasName;
        alias_arg.arg_repetition = eArgRepeatPlain;
        
        // There is only one variant this argument could be; put it into the argument entry.
        arg1.push_back (alias_arg);
        
        // Define the first (and only) variant of this arg.
        cmd_arg.arg_type = eArgTypeCommandName;
        cmd_arg.arg_repetition = eArgRepeatPlain;
        
        // There is only one variant this argument could be; put it into the argument entry.
        arg2.push_back (cmd_arg);
        
        // Define the first (and only) variant of this arg.
        options_arg.arg_type = eArgTypeAliasOptions;
        options_arg.arg_repetition = eArgRepeatOptional;
        
        // There is only one variant this argument could be; put it into the argument entry.
        arg3.push_back (options_arg);
        
        // Push the data for the first argument into the m_arguments vector.
        m_arguments.push_back (arg1);
        m_arguments.push_back (arg2);
        m_arguments.push_back (arg3);
    }

    ~CommandObjectCommandsAlias() override = default;

protected:
    bool
    DoExecute (const char *raw_command_line, CommandReturnObject &result) override
    {
        Args args (raw_command_line);
        std::string raw_command_string (raw_command_line);
        
        size_t argc = args.GetArgumentCount();
        
        if (argc < 2)
        {
            result.AppendError ("'alias' requires at least two arguments");
            result.SetStatus (eReturnStatusFailed);
            return false;
        }
        
        // Get the alias command.
        
        const std::string alias_command = args.GetArgumentAtIndex (0);
        
        // Strip the new alias name off 'raw_command_string'  (leave it on args, which gets passed to 'Execute', which
        // does the stripping itself.
        size_t pos = raw_command_string.find (alias_command);
        if (pos == 0)
        {
            raw_command_string = raw_command_string.substr (alias_command.size());
            pos = raw_command_string.find_first_not_of (' ');
            if ((pos != std::string::npos) && (pos > 0))
                raw_command_string = raw_command_string.substr (pos);
        }
        else
        {
            result.AppendError ("Error parsing command string.  No alias created.");
            result.SetStatus (eReturnStatusFailed);
            return false;
        }
        
        
        // Verify that the command is alias-able.
        if (m_interpreter.CommandExists (alias_command.c_str()))
        {
            result.AppendErrorWithFormat ("'%s' is a permanent debugger command and cannot be redefined.\n",
                                          alias_command.c_str());
            result.SetStatus (eReturnStatusFailed);
            return false;
        }
        
        // Get CommandObject that is being aliased. The command name is read from the front of raw_command_string.
        // raw_command_string is returned with the name of the command object stripped off the front.
        CommandObject *cmd_obj = m_interpreter.GetCommandObjectForCommand (raw_command_string);
        
        if (!cmd_obj)
        {
            result.AppendErrorWithFormat ("invalid command given to 'alias'. '%s' does not begin with a valid command."
                                          "  No alias created.", raw_command_string.c_str());
            result.SetStatus (eReturnStatusFailed);
            return false;
        }
        else if (!cmd_obj->WantsRawCommandString ())
        {
            // Note that args was initialized with the original command, and has not been updated to this point.
            // Therefore can we pass it to the version of Execute that does not need/expect raw input in the alias.
            return HandleAliasingNormalCommand (args, result);
        }
        else
        {
            return HandleAliasingRawCommand (alias_command, raw_command_string, *cmd_obj, result);
        }
        return result.Succeeded();
    }

    bool
    HandleAliasingRawCommand (const std::string &alias_command, std::string &raw_command_string, CommandObject &cmd_obj, CommandReturnObject &result)
    {
            // Verify & handle any options/arguments passed to the alias command
            
            OptionArgVectorSP option_arg_vector_sp = OptionArgVectorSP (new OptionArgVector);
        
            if (CommandObjectSP cmd_obj_sp = m_interpreter.GetCommandSPExact (cmd_obj.GetCommandName(), false))
            {
                if (m_interpreter.AliasExists (alias_command.c_str())
                    || m_interpreter.UserCommandExists (alias_command.c_str()))
                {
                    result.AppendWarningWithFormat ("Overwriting existing definition for '%s'.\n",
                                                    alias_command.c_str());
                }
                if (m_interpreter.AddAlias (alias_command.c_str(), cmd_obj_sp, raw_command_string.c_str()))
                {
                    result.SetStatus (eReturnStatusSuccessFinishNoResult);
                }
                else
                {
                    result.AppendError ("Unable to create requested alias.\n");
                    result.SetStatus (eReturnStatusFailed);
                }

            }
            else
            {
                result.AppendError ("Unable to create requested alias.\n");
                result.SetStatus (eReturnStatusFailed);
            }

            return result.Succeeded ();
    }
    
    bool
    HandleAliasingNormalCommand (Args& args, CommandReturnObject &result)
    {
        size_t argc = args.GetArgumentCount();

        if (argc < 2)
        {
            result.AppendError ("'alias' requires at least two arguments");
            result.SetStatus (eReturnStatusFailed);
            return false;
        }

        const std::string alias_command = args.GetArgumentAtIndex(0);
        const std::string actual_command = args.GetArgumentAtIndex(1);

        args.Shift();  // Shift the alias command word off the argument vector.
        args.Shift();  // Shift the old command word off the argument vector.

        // Verify that the command is alias'able, and get the appropriate command object.

        if (m_interpreter.CommandExists (alias_command.c_str()))
        {
            result.AppendErrorWithFormat ("'%s' is a permanent debugger command and cannot be redefined.\n",
                                         alias_command.c_str());
            result.SetStatus (eReturnStatusFailed);
        }
        else
        {
             CommandObjectSP command_obj_sp(m_interpreter.GetCommandSPExact (actual_command.c_str(), true));
             CommandObjectSP subcommand_obj_sp;
             bool use_subcommand = false;
             if (command_obj_sp)
             {
                 CommandObject *cmd_obj = command_obj_sp.get();
                 CommandObject *sub_cmd_obj = nullptr;
                 OptionArgVectorSP option_arg_vector_sp = OptionArgVectorSP (new OptionArgVector);

                 while (cmd_obj->IsMultiwordObject() && args.GetArgumentCount() > 0)
                 {
                     if (argc >= 3)
                     {
                         const std::string sub_command = args.GetArgumentAtIndex(0);
                         assert (sub_command.length() != 0);
                         subcommand_obj_sp = cmd_obj->GetSubcommandSP (sub_command.c_str());
                         if (subcommand_obj_sp)
                         {
                             sub_cmd_obj = subcommand_obj_sp.get();
                             use_subcommand = true;
                             args.Shift();  // Shift the sub_command word off the argument vector.
                             cmd_obj = sub_cmd_obj;
                         }
                         else
                         {
                             result.AppendErrorWithFormat("'%s' is not a valid sub-command of '%s'.  "
                                                          "Unable to create alias.\n",
                                                          sub_command.c_str(), actual_command.c_str());
                             result.SetStatus (eReturnStatusFailed);
                             return false;
                         }
                     }
                 }

                 // Verify & handle any options/arguments passed to the alias command

                 std::string args_string;
                 
                 if (args.GetArgumentCount () > 0)
                 {
                    CommandObjectSP tmp_sp = m_interpreter.GetCommandSPExact (cmd_obj->GetCommandName(), false);
                    if (use_subcommand)
                        tmp_sp = m_interpreter.GetCommandSPExact (sub_cmd_obj->GetCommandName(), false);
                        
                    args.GetCommandString (args_string);
                 }
                 
                 if (m_interpreter.AliasExists (alias_command.c_str())
                     || m_interpreter.UserCommandExists (alias_command.c_str()))
                 {
                     result.AppendWarningWithFormat ("Overwriting existing definition for '%s'.\n",
                                                     alias_command.c_str());
                 }
                 
                 if (m_interpreter.AddAlias(alias_command.c_str(),
                                            use_subcommand ? subcommand_obj_sp : command_obj_sp,
                                            args_string.c_str()))
                 {
                     result.SetStatus (eReturnStatusSuccessFinishNoResult);
                 }
                 else
                 {
                     result.AppendError ("Unable to create requested alias.\n");
                     result.SetStatus (eReturnStatusFailed);
                     return false;
                 }
             }
             else
             {
                 result.AppendErrorWithFormat ("'%s' is not an existing command.\n", actual_command.c_str());
                 result.SetStatus (eReturnStatusFailed);
                 return false;
             }
        }

        return result.Succeeded();
    }
};

#pragma mark CommandObjectCommandsUnalias
//-------------------------------------------------------------------------
// CommandObjectCommandsUnalias
//-------------------------------------------------------------------------

class CommandObjectCommandsUnalias : public CommandObjectParsed
{
public:
    CommandObjectCommandsUnalias (CommandInterpreter &interpreter) :
        CommandObjectParsed(interpreter,
                            "command unalias",
                            "Allow the user to remove/delete a user-defined command abbreviation.",
                            nullptr)
    {
        CommandArgumentEntry arg;
        CommandArgumentData alias_arg;
        
        // Define the first (and only) variant of this arg.
        alias_arg.arg_type = eArgTypeAliasName;
        alias_arg.arg_repetition = eArgRepeatPlain;
        
        // There is only one variant this argument could be; put it into the argument entry.
        arg.push_back (alias_arg);
        
        // Push the data for the first argument into the m_arguments vector.
        m_arguments.push_back (arg);
    }

    ~CommandObjectCommandsUnalias() override = default;

protected:
    bool
    DoExecute (Args& args, CommandReturnObject &result) override
    {
        CommandObject::CommandMap::iterator pos;
        CommandObject *cmd_obj;

        if (args.GetArgumentCount() != 0)
        {
            const char *command_name = args.GetArgumentAtIndex(0);
            cmd_obj = m_interpreter.GetCommandObject(command_name);
            if (cmd_obj)
            {
                if (m_interpreter.CommandExists (command_name))
                {
                    if (cmd_obj->IsRemovable())
                    {
                        result.AppendErrorWithFormat ("'%s' is not an alias, it is a debugger command which can be removed using the 'command delete' command.\n",
                                                      command_name);
                    }
                    else
                    {
                        result.AppendErrorWithFormat ("'%s' is a permanent debugger command and cannot be removed.\n",
                                                      command_name);
                    }
                    result.SetStatus (eReturnStatusFailed);
                }
                else
                {
                    if (!m_interpreter.RemoveAlias(command_name))
                    {
                        if (m_interpreter.AliasExists (command_name))
                            result.AppendErrorWithFormat ("Error occurred while attempting to unalias '%s'.\n", 
                                                          command_name);
                        else
                            result.AppendErrorWithFormat ("'%s' is not an existing alias.\n", command_name);
                        result.SetStatus (eReturnStatusFailed);
                    }
                    else
                        result.SetStatus (eReturnStatusSuccessFinishNoResult);
                }
            }
            else
            {
                result.AppendErrorWithFormat ("'%s' is not a known command.\nTry 'help' to see a "
                                              "current list of commands.\n",
                                             command_name);
                result.SetStatus (eReturnStatusFailed);
            }
        }
        else
        {
            result.AppendError ("must call 'unalias' with a valid alias");
            result.SetStatus (eReturnStatusFailed);
        }

        return result.Succeeded();
    }
};

#pragma mark CommandObjectCommandsDelete
//-------------------------------------------------------------------------
// CommandObjectCommandsDelete
//-------------------------------------------------------------------------

class CommandObjectCommandsDelete : public CommandObjectParsed
{
public:
    CommandObjectCommandsDelete (CommandInterpreter &interpreter) :
        CommandObjectParsed(interpreter,
                            "command delete",
                            "Allow the user to delete user-defined regular expression, python or multi-word commands.",
                            nullptr)
    {
        CommandArgumentEntry arg;
        CommandArgumentData alias_arg;

        // Define the first (and only) variant of this arg.
        alias_arg.arg_type = eArgTypeCommandName;
        alias_arg.arg_repetition = eArgRepeatPlain;

        // There is only one variant this argument could be; put it into the argument entry.
        arg.push_back (alias_arg);

        // Push the data for the first argument into the m_arguments vector.
        m_arguments.push_back (arg);
    }

    ~CommandObjectCommandsDelete() override = default;

protected:
    bool
    DoExecute (Args& args, CommandReturnObject &result) override
    {
        CommandObject::CommandMap::iterator pos;

        if (args.GetArgumentCount() != 0)
        {
            const char *command_name = args.GetArgumentAtIndex(0);
            if (m_interpreter.CommandExists (command_name))
            {
                if (m_interpreter.RemoveCommand (command_name))
                {
                    result.SetStatus (eReturnStatusSuccessFinishNoResult);
                }
                else
                {
                    result.AppendErrorWithFormat ("'%s' is a permanent debugger command and cannot be removed.\n",
                                                  command_name);
                    result.SetStatus (eReturnStatusFailed);
                }
            }
            else
            {
                StreamString error_msg_stream;
                const bool generate_apropos = true;
                const bool generate_type_lookup = false;
                CommandObjectHelp::GenerateAdditionalHelpAvenuesMessage(&error_msg_stream,
                                                                        command_name,
                                                                        nullptr,
                                                                        nullptr,
                                                                        generate_apropos,
                                                                        generate_type_lookup);
                result.AppendErrorWithFormat ("%s", error_msg_stream.GetData());
                result.SetStatus (eReturnStatusFailed);
            }
        }
        else
        {
            result.AppendErrorWithFormat ("must call '%s' with one or more valid user defined regular expression, python or multi-word command names", GetCommandName ());
            result.SetStatus (eReturnStatusFailed);
        }

        return result.Succeeded();
    }
};

//-------------------------------------------------------------------------
// CommandObjectCommandsAddRegex
//-------------------------------------------------------------------------
#pragma mark CommandObjectCommandsAddRegex

class CommandObjectCommandsAddRegex :
    public CommandObjectParsed,
    public IOHandlerDelegateMultiline
{
public:
    CommandObjectCommandsAddRegex (CommandInterpreter &interpreter) :
        CommandObjectParsed (interpreter,
                       "command regex",
                       "Allow the user to create a regular expression command.",
                       "command regex <cmd-name> [s/<regex>/<subst>/ ...]"),
        IOHandlerDelegateMultiline ("", IOHandlerDelegate::Completion::LLDBCommand),
        m_options (interpreter)
    {
        SetHelpLong(R"(
)" "This command allows the user to create powerful regular expression commands \
with substitutions. The regular expressions and substitutions are specified \
using the regular expression substitution format of:" R"(

    s/<regex>/<subst>/

)" "<regex> is a regular expression that can use parenthesis to capture regular \
expression input and substitute the captured matches in the output using %1 \
for the first match, %2 for the second, and so on." R"(

)" "The regular expressions can all be specified on the command line if more than \
one argument is provided. If just the command name is provided on the command \
line, then the regular expressions and substitutions can be entered on separate \
lines, followed by an empty line to terminate the command definition." R"(

EXAMPLES

)" "The following example will define a regular expression command named 'f' that \
will call 'finish' if there are no arguments, or 'frame select <frame-idx>' if \
a number follows 'f':" R"(

    (lldb) command regex f s/^$/finish/ 's/([0-9]+)/frame select %1/')"
        );
    }

    ~CommandObjectCommandsAddRegex() override = default;

protected:
    void
    IOHandlerActivated (IOHandler &io_handler) override
    {
        StreamFileSP output_sp(io_handler.GetOutputStreamFile());
        if (output_sp)
        {
            output_sp->PutCString("Enter one of more sed substitution commands in the form: 's/<regex>/<subst>/'.\nTerminate the substitution list with an empty line.\n");
            output_sp->Flush();
        }
    }

    void
    IOHandlerInputComplete (IOHandler &io_handler, std::string &data) override
    {
        io_handler.SetIsDone(true);
        if (m_regex_cmd_ap)
        {
            StringList lines;
            if (lines.SplitIntoLines (data))
            {
                const size_t num_lines = lines.GetSize();
                bool check_only = false;
                for (size_t i=0; i<num_lines; ++i)
                {
                    llvm::StringRef bytes_strref (lines[i]);
                    Error error = AppendRegexSubstitution (bytes_strref, check_only);
                    if (error.Fail())
                    {
                        if (!m_interpreter.GetDebugger().GetCommandInterpreter().GetBatchCommandMode())
                        {
                            StreamSP out_stream = m_interpreter.GetDebugger().GetAsyncOutputStream();
                            out_stream->Printf("error: %s\n", error.AsCString());
                        }
                    }
                }
            }
            if (m_regex_cmd_ap->HasRegexEntries())
            {
                CommandObjectSP cmd_sp (m_regex_cmd_ap.release());
                m_interpreter.AddCommand(cmd_sp->GetCommandName(), cmd_sp, true);
            }
        }
    }

    bool
    DoExecute (Args& command, CommandReturnObject &result) override
    {
        const size_t argc = command.GetArgumentCount();
        if (argc == 0)
        {
            result.AppendError ("usage: 'command regex <command-name> [s/<regex1>/<subst1>/ s/<regex2>/<subst2>/ ...]'\n");
            result.SetStatus (eReturnStatusFailed);
        }
        else
        {   
            Error error;
            const char *name = command.GetArgumentAtIndex(0);
            m_regex_cmd_ap.reset (new CommandObjectRegexCommand (m_interpreter, 
                                                                 name, 
                                                                 m_options.GetHelp (),
                                                                 m_options.GetSyntax (),
                                                                 10,
                                                                 0,
                                                                 true));

            if (argc == 1)
            {
                Debugger &debugger = m_interpreter.GetDebugger();
                bool color_prompt = debugger.GetUseColor();
                const bool multiple_lines = true; // Get multiple lines
                IOHandlerSP io_handler_sp(new IOHandlerEditline(debugger,
                                                                IOHandler::Type::Other,
                                                                "lldb-regex", // Name of input reader for history
                                                                "> ",         // Prompt
                                                                nullptr,      // Continuation prompt
                                                                multiple_lines,
                                                                color_prompt,
                                                                0,            // Don't show line numbers
                                                                *this));
                
                if (io_handler_sp)
                {
                    debugger.PushIOHandler(io_handler_sp);
                    result.SetStatus (eReturnStatusSuccessFinishNoResult);
                }
            }
            else
            {
                for (size_t arg_idx = 1; arg_idx < argc; ++arg_idx)
                {
                    llvm::StringRef arg_strref (command.GetArgumentAtIndex(arg_idx));
                    bool check_only = false;
                    error = AppendRegexSubstitution (arg_strref, check_only);
                    if (error.Fail())
                        break;
                }
                
                if (error.Success())
                {
                    AddRegexCommandToInterpreter();
                }
            }
            if (error.Fail())
            {
                result.AppendError (error.AsCString());
                result.SetStatus (eReturnStatusFailed);
            }
        }

        return result.Succeeded();
    }
    
    Error
    AppendRegexSubstitution (const llvm::StringRef &regex_sed, bool check_only)
    {
        Error error;
        
        if (!m_regex_cmd_ap)
        {
            error.SetErrorStringWithFormat("invalid regular expression command object for: '%.*s'", 
                                           (int)regex_sed.size(), 
                                           regex_sed.data());
            return error;
        }
    
        size_t regex_sed_size = regex_sed.size();
        
        if (regex_sed_size <= 1)
        {
            error.SetErrorStringWithFormat("regular expression substitution string is too short: '%.*s'", 
                                           (int)regex_sed.size(), 
                                           regex_sed.data());
            return error;
        }

        if (regex_sed[0] != 's')
        {
            error.SetErrorStringWithFormat("regular expression substitution string doesn't start with 's': '%.*s'", 
                                           (int)regex_sed.size(), 
                                           regex_sed.data());
            return error;
        }
        const size_t first_separator_char_pos = 1;
        // use the char that follows 's' as the regex separator character
        // so we can have "s/<regex>/<subst>/" or "s|<regex>|<subst>|"
        const char separator_char = regex_sed[first_separator_char_pos];
        const size_t second_separator_char_pos = regex_sed.find (separator_char, first_separator_char_pos + 1);
        
        if (second_separator_char_pos == std::string::npos)
        {
            error.SetErrorStringWithFormat("missing second '%c' separator char after '%.*s' in '%.*s'",
                                           separator_char, 
                                           (int)(regex_sed.size() - first_separator_char_pos - 1),
                                           regex_sed.data() + (first_separator_char_pos + 1),
                                           (int)regex_sed.size(),
                                           regex_sed.data());
            return error;
        }

        const size_t third_separator_char_pos = regex_sed.find (separator_char, second_separator_char_pos + 1);
        
        if (third_separator_char_pos == std::string::npos)
        {
            error.SetErrorStringWithFormat("missing third '%c' separator char after '%.*s' in '%.*s'",
                                           separator_char, 
                                           (int)(regex_sed.size() - second_separator_char_pos - 1),
                                           regex_sed.data() + (second_separator_char_pos + 1),
                                           (int)regex_sed.size(),
                                           regex_sed.data());
            return error;            
        }

        if (third_separator_char_pos != regex_sed_size - 1)
        {
            // Make sure that everything that follows the last regex 
            // separator char 
            if (regex_sed.find_first_not_of("\t\n\v\f\r ", third_separator_char_pos + 1) != std::string::npos)
            {
                error.SetErrorStringWithFormat("extra data found after the '%.*s' regular expression substitution string: '%.*s'", 
                                               (int)third_separator_char_pos + 1,
                                               regex_sed.data(),
                                               (int)(regex_sed.size() - third_separator_char_pos - 1),
                                               regex_sed.data() + (third_separator_char_pos + 1));
                return error;
            }
        }
        else if (first_separator_char_pos + 1 == second_separator_char_pos)
        {
            error.SetErrorStringWithFormat("<regex> can't be empty in 's%c<regex>%c<subst>%c' string: '%.*s'",  
                                           separator_char,
                                           separator_char,
                                           separator_char,
                                           (int)regex_sed.size(), 
                                           regex_sed.data());
            return error;            
        }
        else if (second_separator_char_pos + 1 == third_separator_char_pos)
        {
            error.SetErrorStringWithFormat("<subst> can't be empty in 's%c<regex>%c<subst>%c' string: '%.*s'",   
                                           separator_char,
                                           separator_char,
                                           separator_char,
                                           (int)regex_sed.size(), 
                                           regex_sed.data());
            return error;            
        }

        if (!check_only)
        {
            std::string regex(regex_sed.substr(first_separator_char_pos + 1, second_separator_char_pos - first_separator_char_pos - 1));
            std::string subst(regex_sed.substr(second_separator_char_pos + 1, third_separator_char_pos - second_separator_char_pos - 1));
            m_regex_cmd_ap->AddRegexCommand (regex.c_str(),
                                             subst.c_str());
        }
        return error;
    }
    
    void
    AddRegexCommandToInterpreter()
    {
        if (m_regex_cmd_ap)
        {
            if (m_regex_cmd_ap->HasRegexEntries())
            {
                CommandObjectSP cmd_sp (m_regex_cmd_ap.release());
                m_interpreter.AddCommand(cmd_sp->GetCommandName(), cmd_sp, true);
            }
        }
    }

private:
    std::unique_ptr<CommandObjectRegexCommand> m_regex_cmd_ap;

     class CommandOptions : public Options
     {
     public:
         CommandOptions (CommandInterpreter &interpreter) :
            Options (interpreter)
         {
         }

         ~CommandOptions() override = default;

         Error
         SetOptionValue (uint32_t option_idx, const char *option_arg) override
         {
             Error error;
             const int short_option = m_getopt_table[option_idx].val;
             
             switch (short_option)
             {
                 case 'h':
                     m_help.assign (option_arg);
                     break;
                 case 's':
                     m_syntax.assign (option_arg);
                     break;
                 default:
                     error.SetErrorStringWithFormat ("unrecognized option '%c'", short_option);
                     break;
             }
             
             return error;
         }
         
         void
         OptionParsingStarting () override
         {
             m_help.clear();
             m_syntax.clear();
         }
         
         const OptionDefinition*
         GetDefinitions () override
         {
             return g_option_table;
         }
         
         // Options table: Required for subclasses of Options.
         
         static OptionDefinition g_option_table[];
         
         const char *
         GetHelp()
         {
             return (m_help.empty() ? nullptr : m_help.c_str());
         }

         const char *
         GetSyntax ()
         {
             return (m_syntax.empty() ? nullptr : m_syntax.c_str());
         }

     protected:
         // Instance variables to hold the values for command options.

         std::string m_help;
         std::string m_syntax;
     };
          
     Options *
     GetOptions () override
     {
         return &m_options;
     }
     
     CommandOptions m_options;
};

OptionDefinition
CommandObjectCommandsAddRegex::CommandOptions::g_option_table[] =
{
{ LLDB_OPT_SET_1, false, "help"  , 'h', OptionParser::eRequiredArgument, nullptr, nullptr, 0, eArgTypeNone, "The help text to display for this command."},
{ LLDB_OPT_SET_1, false, "syntax", 's', OptionParser::eRequiredArgument, nullptr, nullptr, 0, eArgTypeNone, "A syntax string showing the typical usage syntax."},
{ 0             , false,  nullptr   , 0  , 0                , nullptr, nullptr, 0, eArgTypeNone, nullptr }
};

class CommandObjectPythonFunction : public CommandObjectRaw
{
public:
    CommandObjectPythonFunction (CommandInterpreter &interpreter,
                                 std::string name,
                                 std::string funct,
                                 std::string help,
                                 ScriptedCommandSynchronicity synch) :
        CommandObjectRaw(interpreter,
                         name.c_str(),
                         nullptr,
                         nullptr),
        m_function_name(funct),
        m_synchro(synch),
        m_fetched_help_long(false)
    {
        if (!help.empty())
            SetHelp(help.c_str());
        else
        {
            StreamString stream;
            stream.Printf("For more information run 'help %s'",name.c_str());
            SetHelp(stream.GetData());
        }
    }

    ~CommandObjectPythonFunction() override = default;

    bool
    IsRemovable () const override
    {
        return true;
    }

    const std::string&
    GetFunctionName ()
    {
        return m_function_name;
    }

    ScriptedCommandSynchronicity
    GetSynchronicity ()
    {
        return m_synchro;
    }
    
    const char *
    GetHelpLong () override
    {
        if (!m_fetched_help_long)
        {
            ScriptInterpreter* scripter = m_interpreter.GetScriptInterpreter();
            if (scripter)
            {
                std::string docstring;
                m_fetched_help_long = scripter->GetDocumentationForItem(m_function_name.c_str(),docstring);
                if (!docstring.empty())
                    SetHelpLong(docstring);
            }
        }
        return CommandObjectRaw::GetHelpLong();
    }
    
protected:
    bool
    DoExecute (const char *raw_command_line, CommandReturnObject &result) override
    {
        ScriptInterpreter* scripter = m_interpreter.GetScriptInterpreter();
        
        Error error;
        
        result.SetStatus(eReturnStatusInvalid);
        
        if (!scripter || !scripter->RunScriptBasedCommand(m_function_name.c_str(),
                                                          raw_command_line,
                                                          m_synchro,
                                                          result,
                                                          error,
                                                          m_exe_ctx))
        {
            result.AppendError(error.AsCString());
            result.SetStatus(eReturnStatusFailed);
        }
        else
        {
            // Don't change the status if the command already set it...
            if (result.GetStatus() == eReturnStatusInvalid)
            {
                if (result.GetOutputData() == nullptr || result.GetOutputData()[0] == '\0')
                    result.SetStatus(eReturnStatusSuccessFinishNoResult);
                else
                    result.SetStatus(eReturnStatusSuccessFinishResult);
            }
        }
        
        return result.Succeeded();
    }

private:
    std::string m_function_name;
    ScriptedCommandSynchronicity m_synchro;
    bool m_fetched_help_long;
};

class CommandObjectScriptingObject : public CommandObjectRaw
{
public:
    CommandObjectScriptingObject (CommandInterpreter &interpreter,
                                  std::string name,
                                  StructuredData::GenericSP cmd_obj_sp,
                                  ScriptedCommandSynchronicity synch) :
        CommandObjectRaw(interpreter,
                         name.c_str(),
                         nullptr,
                         nullptr),
        m_cmd_obj_sp(cmd_obj_sp),
        m_synchro(synch),
        m_fetched_help_short(false),
        m_fetched_help_long(false)
    {
        StreamString stream;
        stream.Printf("For more information run 'help %s'",name.c_str());
        SetHelp(stream.GetData());
        if (ScriptInterpreter* scripter = m_interpreter.GetScriptInterpreter())
            GetFlags().Set(scripter->GetFlagsForCommandObject(cmd_obj_sp));
    }

    ~CommandObjectScriptingObject() override = default;

    bool
    IsRemovable () const override
    {
        return true;
    }
    
    StructuredData::GenericSP
    GetImplementingObject ()
    {
        return m_cmd_obj_sp;
    }
    
    ScriptedCommandSynchronicity
    GetSynchronicity ()
    {
        return m_synchro;
    }

    const char *
    GetHelp () override
    {
        if (!m_fetched_help_short)
        {
            ScriptInterpreter* scripter = m_interpreter.GetScriptInterpreter();
            if (scripter)
            {
                std::string docstring;
                m_fetched_help_short = scripter->GetShortHelpForCommandObject(m_cmd_obj_sp,docstring);
                if (!docstring.empty())
                    SetHelp(docstring);
            }
        }
        return CommandObjectRaw::GetHelp();
    }
    
    const char *
    GetHelpLong () override
    {
        if (!m_fetched_help_long)
        {
            ScriptInterpreter* scripter = m_interpreter.GetScriptInterpreter();
            if (scripter)
            {
                std::string docstring;
                m_fetched_help_long = scripter->GetLongHelpForCommandObject(m_cmd_obj_sp,docstring);
                if (!docstring.empty())
                    SetHelpLong(docstring);
            }
        }
        return CommandObjectRaw::GetHelpLong();
    }
    
protected:
    bool
    DoExecute (const char *raw_command_line, CommandReturnObject &result) override
    {
        ScriptInterpreter* scripter = m_interpreter.GetScriptInterpreter();
        
        Error error;
        
        result.SetStatus(eReturnStatusInvalid);
        
        if (!scripter || !scripter->RunScriptBasedCommand(m_cmd_obj_sp,
                                                          raw_command_line,
                                                          m_synchro,
                                                          result,
                                                          error,
                                                          m_exe_ctx))
        {
            result.AppendError(error.AsCString());
            result.SetStatus(eReturnStatusFailed);
        }
        else
        {
            // Don't change the status if the command already set it...
            if (result.GetStatus() == eReturnStatusInvalid)
            {
                if (result.GetOutputData() == nullptr || result.GetOutputData()[0] == '\0')
                    result.SetStatus(eReturnStatusSuccessFinishNoResult);
                else
                    result.SetStatus(eReturnStatusSuccessFinishResult);
            }
        }
        
        return result.Succeeded();
    }

private:
    StructuredData::GenericSP m_cmd_obj_sp;
    ScriptedCommandSynchronicity m_synchro;
    bool m_fetched_help_short: 1;
    bool m_fetched_help_long: 1;
};

//-------------------------------------------------------------------------
// CommandObjectCommandsScriptImport
//-------------------------------------------------------------------------

class CommandObjectCommandsScriptImport : public CommandObjectParsed
{
public:
    CommandObjectCommandsScriptImport (CommandInterpreter &interpreter) :
        CommandObjectParsed(interpreter,
                            "command script import",
                            "Import a scripting module in LLDB.",
                            nullptr),
        m_options(interpreter)
    {
        CommandArgumentEntry arg1;
        CommandArgumentData cmd_arg;
        
        // Define the first (and only) variant of this arg.
        cmd_arg.arg_type = eArgTypeFilename;
        cmd_arg.arg_repetition = eArgRepeatPlus;
        
        // There is only one variant this argument could be; put it into the argument entry.
        arg1.push_back (cmd_arg);
        
        // Push the data for the first argument into the m_arguments vector.
        m_arguments.push_back (arg1);
    }

    ~CommandObjectCommandsScriptImport() override = default;

    int
    HandleArgumentCompletion (Args &input,
                              int &cursor_index,
                              int &cursor_char_position,
                              OptionElementVector &opt_element_vector,
                              int match_start_point,
                              int max_return_elements,
                              bool &word_complete,
                              StringList &matches) override
    {
        std::string completion_str (input.GetArgumentAtIndex(cursor_index));
        completion_str.erase (cursor_char_position);
        
        CommandCompletions::InvokeCommonCompletionCallbacks(m_interpreter,
                                                            CommandCompletions::eDiskFileCompletion,
                                                            completion_str.c_str(),
                                                            match_start_point,
                                                            max_return_elements,
                                                            nullptr,
                                                            word_complete,
                                                            matches);
        return matches.GetSize();
    }
    
    Options *
    GetOptions () override
    {
        return &m_options;
    }

protected:
    class CommandOptions : public Options
    {
    public:
        CommandOptions (CommandInterpreter &interpreter) :
            Options (interpreter)
        {
        }

        ~CommandOptions() override = default;

        Error
        SetOptionValue (uint32_t option_idx, const char *option_arg) override
        {
            Error error;
            const int short_option = m_getopt_table[option_idx].val;
            
            switch (short_option)
            {
                case 'r':
                    m_allow_reload = true;
                    break;
                default:
                    error.SetErrorStringWithFormat ("unrecognized option '%c'", short_option);
                    break;
            }
            
            return error;
        }
        
        void
        OptionParsingStarting () override
        {
            m_allow_reload = true;
        }
        
        const OptionDefinition*
        GetDefinitions () override
        {
            return g_option_table;
        }
        
        // Options table: Required for subclasses of Options.
        
        static OptionDefinition g_option_table[];
        
        // Instance variables to hold the values for command options.
        
        bool m_allow_reload;
    };

    bool
    DoExecute (Args& command, CommandReturnObject &result) override
    {
        if (m_interpreter.GetDebugger().GetScriptLanguage() != lldb::eScriptLanguagePython)
        {
            result.AppendError ("only scripting language supported for module importing is currently Python");
            result.SetStatus (eReturnStatusFailed);
            return false;
        }
        
        size_t argc = command.GetArgumentCount();
        if (0 == argc)
        {
            result.AppendError("command script import needs one or more arguments");
            result.SetStatus (eReturnStatusFailed);
            return false;
        }
        
        for (size_t i = 0;
             i < argc;
             i++)
        {
            std::string path = command.GetArgumentAtIndex(i);
            Error error;
            
            const bool init_session = true;
            // FIXME: this is necessary because CommandObject::CheckRequirements() assumes that
            // commands won't ever be recursively invoked, but it's actually possible to craft
            // a Python script that does other "command script imports" in __lldb_init_module
            // the real fix is to have recursive commands possible with a CommandInvocation object
            // separate from the CommandObject itself, so that recursive command invocations
            // won't stomp on each other (wrt to execution contents, options, and more)
            m_exe_ctx.Clear();
            if (m_interpreter.GetScriptInterpreter()->LoadScriptingModule(path.c_str(),
                                                                          m_options.m_allow_reload,
                                                                          init_session,
                                                                          error))
            {
                result.SetStatus (eReturnStatusSuccessFinishNoResult);
            }
            else
            {
                result.AppendErrorWithFormat("module importing failed: %s", error.AsCString());
                result.SetStatus (eReturnStatusFailed);
            }
        }
        
        return result.Succeeded();
    }
    
    CommandOptions m_options;
};

OptionDefinition
CommandObjectCommandsScriptImport::CommandOptions::g_option_table[] =
{
    { LLDB_OPT_SET_1, false, "allow-reload", 'r', OptionParser::eNoArgument, nullptr, nullptr, 0, eArgTypeNone,        "Allow the script to be loaded even if it was already loaded before. This argument exists for backwards compatibility, but reloading is always allowed, whether you specify it or not."},
    { 0, false, nullptr, 0, 0, nullptr, nullptr, 0, eArgTypeNone, nullptr }
};

//-------------------------------------------------------------------------
// CommandObjectCommandsScriptAdd
//-------------------------------------------------------------------------

class CommandObjectCommandsScriptAdd :
    public CommandObjectParsed,
    public IOHandlerDelegateMultiline
{
public:
    CommandObjectCommandsScriptAdd(CommandInterpreter &interpreter) :
        CommandObjectParsed(interpreter,
                            "command script add",
                            "Add a scripted function as an LLDB command.",
                            nullptr),
        IOHandlerDelegateMultiline ("DONE"),
        m_options (interpreter)
    {
        CommandArgumentEntry arg1;
        CommandArgumentData cmd_arg;
        
        // Define the first (and only) variant of this arg.
        cmd_arg.arg_type = eArgTypeCommandName;
        cmd_arg.arg_repetition = eArgRepeatPlain;
        
        // There is only one variant this argument could be; put it into the argument entry.
        arg1.push_back (cmd_arg);
        
        // Push the data for the first argument into the m_arguments vector.
        m_arguments.push_back (arg1);
    }

    ~CommandObjectCommandsScriptAdd() override = default;

    Options *
    GetOptions () override
    {
        return &m_options;
    }
    
protected:
    class CommandOptions : public Options
    {
    public:
        CommandOptions (CommandInterpreter &interpreter) :
            Options (interpreter),
            m_class_name(),
            m_funct_name(),
            m_short_help(),
            m_synchronicity(eScriptedCommandSynchronicitySynchronous)
        {
        }

        ~CommandOptions() override = default;

        Error
        SetOptionValue (uint32_t option_idx, const char *option_arg) override
        {
            Error error;
            const int short_option = m_getopt_table[option_idx].val;
            
            switch (short_option)
            {
                case 'f':
                    if (option_arg)
                        m_funct_name.assign(option_arg);
                    break;
                case 'c':
                    if (option_arg)
                        m_class_name.assign(option_arg);
                    break;
                case 'h':
                    if (option_arg)
                        m_short_help.assign(option_arg);
                    break;
                case 's':
                    m_synchronicity = (ScriptedCommandSynchronicity) Args::StringToOptionEnum(option_arg, g_option_table[option_idx].enum_values, 0, error);
                    if (!error.Success())
                        error.SetErrorStringWithFormat ("unrecognized value for synchronicity '%s'", option_arg);
                    break;
                default:
                    error.SetErrorStringWithFormat ("unrecognized option '%c'", short_option);
                    break;
            }
            
            return error;
        }
        
        void
        OptionParsingStarting () override
        {
            m_class_name.clear();
            m_funct_name.clear();
            m_short_help.clear();
            m_synchronicity = eScriptedCommandSynchronicitySynchronous;
        }
        
        const OptionDefinition*
        GetDefinitions () override
        {
            return g_option_table;
        }
        
        // Options table: Required for subclasses of Options.
        
        static OptionDefinition g_option_table[];
        
        // Instance variables to hold the values for command options.
        
        std::string m_class_name;
        std::string m_funct_name;
        std::string m_short_help;
        ScriptedCommandSynchronicity m_synchronicity;
    };

    void
    IOHandlerActivated (IOHandler &io_handler) override
    {
        StreamFileSP output_sp(io_handler.GetOutputStreamFile());
        if (output_sp)
        {
            output_sp->PutCString(g_python_command_instructions);
            output_sp->Flush();
        }
    }
    

    void
    IOHandlerInputComplete (IOHandler &io_handler, std::string &data) override
    {
        StreamFileSP error_sp = io_handler.GetErrorStreamFile();
        
        ScriptInterpreter *interpreter = m_interpreter.GetScriptInterpreter();
        if (interpreter)
        {
        
            StringList lines;
            lines.SplitIntoLines(data);
            if (lines.GetSize() > 0)
            {
                std::string funct_name_str;
                if (interpreter->GenerateScriptAliasFunction (lines, funct_name_str))
                {
                    if (funct_name_str.empty())
                    {
                        error_sp->Printf ("error: unable to obtain a function name, didn't add python command.\n");
                        error_sp->Flush();
                    }
                    else
                    {
                        // everything should be fine now, let's add this alias
                        
                        CommandObjectSP command_obj_sp(new CommandObjectPythonFunction (m_interpreter,
                                                                                        m_cmd_name,
                                                                                        funct_name_str.c_str(),
                                                                                        m_short_help,
                                                                                        m_synchronicity));
                        
                        if (!m_interpreter.AddUserCommand(m_cmd_name, command_obj_sp, true))
                        {
                            error_sp->Printf ("error: unable to add selected command, didn't add python command.\n");
                            error_sp->Flush();
                        }
                    }
                }
                else
                {
                    error_sp->Printf ("error: unable to create function, didn't add python command.\n");
                    error_sp->Flush();
                }
            }
            else
            {
                error_sp->Printf ("error: empty function, didn't add python command.\n");
                error_sp->Flush();
            }
        }
        else
        {
            error_sp->Printf ("error: script interpreter missing, didn't add python command.\n");
            error_sp->Flush();
        }

        io_handler.SetIsDone(true);
    }

protected:
    bool
    DoExecute (Args& command, CommandReturnObject &result) override
    {
        if (m_interpreter.GetDebugger().GetScriptLanguage() != lldb::eScriptLanguagePython)
        {
            result.AppendError ("only scripting language supported for scripted commands is currently Python");
            result.SetStatus (eReturnStatusFailed);
            return false;
        }
        
        size_t argc = command.GetArgumentCount();
        
        if (argc != 1)
        {
            result.AppendError ("'command script add' requires one argument");
            result.SetStatus (eReturnStatusFailed);
            return false;
        }
        
        // Store the options in case we get multi-line input
        m_cmd_name = command.GetArgumentAtIndex(0);
        m_short_help.assign(m_options.m_short_help);
        m_synchronicity = m_options.m_synchronicity;
        
        if (m_options.m_class_name.empty())
        {
            if (m_options.m_funct_name.empty())
            {
                m_interpreter.GetPythonCommandsFromIOHandler("     ",  // Prompt
                                                             *this,    // IOHandlerDelegate
                                                             true,     // Run IOHandler in async mode
                                                             nullptr); // Baton for the "io_handler" that will be passed back into our IOHandlerDelegate functions
            }
            else
            {
                CommandObjectSP new_cmd(new CommandObjectPythonFunction(m_interpreter,
                                                                        m_cmd_name,
                                                                        m_options.m_funct_name,
                                                                        m_options.m_short_help,
                                                                        m_synchronicity));
                if (m_interpreter.AddUserCommand(m_cmd_name, new_cmd, true))
                {
                    result.SetStatus (eReturnStatusSuccessFinishNoResult);
                }
                else
                {
                    result.AppendError("cannot add command");
                    result.SetStatus (eReturnStatusFailed);
                }
            }
        }
        else
        {
            ScriptInterpreter *interpreter = GetCommandInterpreter().GetScriptInterpreter();
            if (!interpreter)
            {
                result.AppendError("cannot find ScriptInterpreter");
                result.SetStatus(eReturnStatusFailed);
                return false;
            }
            
            auto cmd_obj_sp = interpreter->CreateScriptCommandObject(m_options.m_class_name.c_str());
            if (!cmd_obj_sp)
            {
                result.AppendError("cannot create helper object");
                result.SetStatus(eReturnStatusFailed);
                return false;
            }
            
            CommandObjectSP new_cmd(new CommandObjectScriptingObject(m_interpreter,
                                                                     m_cmd_name,
                                                                     cmd_obj_sp,
                                                                     m_synchronicity));
            if (m_interpreter.AddUserCommand(m_cmd_name, new_cmd, true))
            {
                result.SetStatus (eReturnStatusSuccessFinishNoResult);
            }
            else
            {
                result.AppendError("cannot add command");
                result.SetStatus (eReturnStatusFailed);
            }
        }

        return result.Succeeded();
    }
    
    CommandOptions m_options;
    std::string m_cmd_name;
    std::string m_short_help;
    ScriptedCommandSynchronicity m_synchronicity;
};

static OptionEnumValueElement g_script_synchro_type[] =
{
    { eScriptedCommandSynchronicitySynchronous,      "synchronous",       "Run synchronous"},
    { eScriptedCommandSynchronicityAsynchronous,     "asynchronous",      "Run asynchronous"},
    { eScriptedCommandSynchronicityCurrentValue,     "current",           "Do not alter current setting"},
    { 0, nullptr, nullptr }
};

OptionDefinition
CommandObjectCommandsScriptAdd::CommandOptions::g_option_table[] =
{
    { LLDB_OPT_SET_1, false, "function", 'f', OptionParser::eRequiredArgument, nullptr, nullptr, 0, eArgTypePythonFunction,        "Name of the Python function to bind to this command name."},
    { LLDB_OPT_SET_2, false, "class", 'c', OptionParser::eRequiredArgument, nullptr, nullptr, 0, eArgTypePythonClass,        "Name of the Python class to bind to this command name."},
    { LLDB_OPT_SET_1, false, "help"  , 'h', OptionParser::eRequiredArgument, nullptr, nullptr, 0, eArgTypeHelpText, "The help text to display for this command."},
    { LLDB_OPT_SET_ALL, false, "synchronicity", 's', OptionParser::eRequiredArgument, nullptr, g_script_synchro_type, 0, eArgTypeScriptedCommandSynchronicity,        "Set the synchronicity of this command's executions with regard to LLDB event system."},
    { 0, false, nullptr, 0, 0, nullptr, nullptr, 0, eArgTypeNone, nullptr }
};

//-------------------------------------------------------------------------
// CommandObjectCommandsScriptList
//-------------------------------------------------------------------------

class CommandObjectCommandsScriptList : public CommandObjectParsed
{
public:
    CommandObjectCommandsScriptList(CommandInterpreter &interpreter) :
        CommandObjectParsed(interpreter,
                            "command script list",
                            "List defined scripted commands.",
                            nullptr)
    {
    }

    ~CommandObjectCommandsScriptList() override = default;

    bool
    DoExecute (Args& command, CommandReturnObject &result) override
    {
        m_interpreter.GetHelp(result,
                              CommandInterpreter::eCommandTypesUserDef);
        
        result.SetStatus (eReturnStatusSuccessFinishResult);
        
        return true;
    }
};

//-------------------------------------------------------------------------
// CommandObjectCommandsScriptClear
//-------------------------------------------------------------------------

class CommandObjectCommandsScriptClear : public CommandObjectParsed
{
public:
    CommandObjectCommandsScriptClear(CommandInterpreter &interpreter) :
        CommandObjectParsed(interpreter,
                            "command script clear",
                            "Delete all scripted commands.",
                            nullptr)
    {
    }

    ~CommandObjectCommandsScriptClear() override = default;

protected:
    bool
    DoExecute (Args& command, CommandReturnObject &result) override
    {
        m_interpreter.RemoveAllUser();
        
        result.SetStatus (eReturnStatusSuccessFinishResult);
        
        return true;
    }
};

//-------------------------------------------------------------------------
// CommandObjectCommandsScriptDelete
//-------------------------------------------------------------------------

class CommandObjectCommandsScriptDelete : public CommandObjectParsed
{
public:
    CommandObjectCommandsScriptDelete(CommandInterpreter &interpreter) :
        CommandObjectParsed(interpreter,
                            "command script delete",
                            "Delete a scripted command.",
                            nullptr)
    {
        CommandArgumentEntry arg1;
        CommandArgumentData cmd_arg;
        
        // Define the first (and only) variant of this arg.
        cmd_arg.arg_type = eArgTypeCommandName;
        cmd_arg.arg_repetition = eArgRepeatPlain;
        
        // There is only one variant this argument could be; put it into the argument entry.
        arg1.push_back (cmd_arg);
        
        // Push the data for the first argument into the m_arguments vector.
        m_arguments.push_back (arg1);
    }

    ~CommandObjectCommandsScriptDelete() override = default;

protected:
    bool
    DoExecute (Args& command, CommandReturnObject &result) override
    {
        
        size_t argc = command.GetArgumentCount();
        
        if (argc != 1)
        {
            result.AppendError ("'command script delete' requires one argument");
            result.SetStatus (eReturnStatusFailed);
            return false;
        }
        
        const char* cmd_name = command.GetArgumentAtIndex(0);
        
        if (cmd_name && *cmd_name && m_interpreter.HasUserCommands() && m_interpreter.UserCommandExists(cmd_name))
        {
            m_interpreter.RemoveUser(cmd_name);
            result.SetStatus (eReturnStatusSuccessFinishResult);
        }
        else
        {
            result.AppendErrorWithFormat ("command %s not found", cmd_name);
            result.SetStatus (eReturnStatusFailed);
        }
        
        return result.Succeeded();
    }
};

#pragma mark CommandObjectMultiwordCommandsScript

//-------------------------------------------------------------------------
// CommandObjectMultiwordCommandsScript
//-------------------------------------------------------------------------

class CommandObjectMultiwordCommandsScript : public CommandObjectMultiword
{
public:
    CommandObjectMultiwordCommandsScript (CommandInterpreter &interpreter) :
    CommandObjectMultiword (interpreter,
                            "command script",
                            "A set of commands for managing or customizing script commands.",
                            "command script <subcommand> [<subcommand-options>]")
    {
        LoadSubCommand ("add",    CommandObjectSP (new CommandObjectCommandsScriptAdd (interpreter)));
        LoadSubCommand ("delete", CommandObjectSP (new CommandObjectCommandsScriptDelete (interpreter)));
        LoadSubCommand ("clear",  CommandObjectSP (new CommandObjectCommandsScriptClear (interpreter)));
        LoadSubCommand ("list",   CommandObjectSP (new CommandObjectCommandsScriptList (interpreter)));
        LoadSubCommand ("import", CommandObjectSP (new CommandObjectCommandsScriptImport (interpreter)));
    }

    ~CommandObjectMultiwordCommandsScript() override = default;
};

#pragma mark CommandObjectMultiwordCommands

//-------------------------------------------------------------------------
// CommandObjectMultiwordCommands
//-------------------------------------------------------------------------

CommandObjectMultiwordCommands::CommandObjectMultiwordCommands (CommandInterpreter &interpreter) :
    CommandObjectMultiword (interpreter,
                            "command",
                            "A set of commands for managing or customizing the debugger commands.",
                            "command <subcommand> [<subcommand-options>]")
{
    LoadSubCommand ("source",  CommandObjectSP (new CommandObjectCommandsSource (interpreter)));
    LoadSubCommand ("alias",   CommandObjectSP (new CommandObjectCommandsAlias (interpreter)));
    LoadSubCommand ("unalias", CommandObjectSP (new CommandObjectCommandsUnalias (interpreter)));
    LoadSubCommand ("delete",  CommandObjectSP (new CommandObjectCommandsDelete (interpreter)));
    LoadSubCommand ("regex",   CommandObjectSP (new CommandObjectCommandsAddRegex (interpreter)));
    LoadSubCommand ("history", CommandObjectSP (new CommandObjectCommandsHistory (interpreter)));
    LoadSubCommand ("script",  CommandObjectSP (new CommandObjectMultiwordCommandsScript (interpreter)));
}

CommandObjectMultiwordCommands::~CommandObjectMultiwordCommands() = default;
