//===-- CommandObjectMemory.cpp ---------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// C Includes
#include <inttypes.h>

// C++ Includes
// Other libraries and framework includes
#include "clang/AST/Decl.h"

// Project includes
#include "CommandObjectMemory.h"
#include "lldb/Core/DataBufferHeap.h"
#include "lldb/Core/DataExtractor.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/StreamString.h"
#include "lldb/Core/ValueObjectMemory.h"
#include "lldb/DataFormatters/ValueObjectPrinter.h"
#include "Plugins/ExpressionParser/Clang/ClangPersistentVariables.h"
#include "lldb/Host/StringConvert.h"
#include "lldb/Interpreter/Args.h"
#include "lldb/Interpreter/CommandReturnObject.h"
#include "lldb/Interpreter/CommandInterpreter.h"
#include "lldb/Interpreter/Options.h"
#include "lldb/Interpreter/OptionGroupFormat.h"
#include "lldb/Interpreter/OptionGroupOutputFile.h"
#include "lldb/Interpreter/OptionGroupValueObjectDisplay.h"
#include "lldb/Interpreter/OptionValueString.h"
#include "lldb/Symbol/ClangASTContext.h"
#include "lldb/Symbol/TypeList.h"
#include "lldb/Symbol/SymbolFile.h"
#include "lldb/Target/MemoryHistory.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/StackFrame.h"
#include "lldb/Target/Thread.h"

#include "lldb/lldb-private.h"

using namespace lldb;
using namespace lldb_private;

static OptionDefinition
g_option_table[] =
{
    { LLDB_OPT_SET_1, false, "num-per-line" ,'l', OptionParser::eRequiredArgument, nullptr, nullptr, 0, eArgTypeNumberPerLine ,"The number of items per line to display."},
    { LLDB_OPT_SET_2, false, "binary"       ,'b', OptionParser::eNoArgument      , nullptr, nullptr, 0, eArgTypeNone          ,"If true, memory will be saved as binary. If false, the memory is saved save as an ASCII dump that uses the format, size, count and number per line settings."},
    { LLDB_OPT_SET_3, true , "type"         ,'t', OptionParser::eRequiredArgument, nullptr, nullptr, 0, eArgTypeNone          ,"The name of a type to view memory as."},
    { LLDB_OPT_SET_3, false , "offset"      ,'E', OptionParser::eRequiredArgument, nullptr, nullptr, 0, eArgTypeCount         ,"How many elements of the specified type to skip before starting to display data."},
    { LLDB_OPT_SET_1|
      LLDB_OPT_SET_2|
      LLDB_OPT_SET_3, false, "force"        ,'r', OptionParser::eNoArgument,       nullptr, nullptr, 0, eArgTypeNone          ,"Necessary if reading over target.max-memory-read-size bytes."},
};

class OptionGroupReadMemory : public OptionGroup
{
public:
    OptionGroupReadMemory () :
        m_num_per_line (1,1),
        m_output_as_binary (false),
        m_view_as_type(),
        m_offset(0,0)
    {
    }

    ~OptionGroupReadMemory() override = default;

    uint32_t
    GetNumDefinitions () override
    {
        return sizeof (g_option_table) / sizeof (OptionDefinition);
    }
    
    const OptionDefinition*
    GetDefinitions () override
    {
        return g_option_table;
    }
    
    Error
    SetOptionValue (CommandInterpreter &interpreter,
                    uint32_t option_idx,
                    const char *option_arg) override
    {
        Error error;
        const int short_option = g_option_table[option_idx].short_option;
        
        switch (short_option)
        {
            case 'l':
                error = m_num_per_line.SetValueFromString (option_arg);
                if (m_num_per_line.GetCurrentValue() == 0)
                    error.SetErrorStringWithFormat("invalid value for --num-per-line option '%s'", option_arg);
                break;

            case 'b':
                m_output_as_binary = true;
                break;
                
            case 't':
                error = m_view_as_type.SetValueFromString (option_arg);
                break;
            
            case 'r':
                m_force = true;
                break;
                
            case 'E':
                error = m_offset.SetValueFromString(option_arg);
                break;
                
            default:
                error.SetErrorStringWithFormat("unrecognized short option '%c'", short_option);
                break;
        }
        return error;
    }
    
    void
    OptionParsingStarting (CommandInterpreter &interpreter) override
    {
        m_num_per_line.Clear();
        m_output_as_binary = false;
        m_view_as_type.Clear();
        m_force = false;
        m_offset.Clear();
    }
    
    Error
    FinalizeSettings (Target *target, OptionGroupFormat& format_options)
    {
        Error error;
        OptionValueUInt64 &byte_size_value = format_options.GetByteSizeValue();
        OptionValueUInt64 &count_value = format_options.GetCountValue();
        const bool byte_size_option_set = byte_size_value.OptionWasSet();
        const bool num_per_line_option_set = m_num_per_line.OptionWasSet();
        const bool count_option_set = format_options.GetCountValue().OptionWasSet();
        
        switch (format_options.GetFormat())
        {
            default:
                break;
                
            case eFormatBoolean:
                if (!byte_size_option_set)
                    byte_size_value = 1;
                if (!num_per_line_option_set)
                    m_num_per_line = 1;
                if (!count_option_set)
                    format_options.GetCountValue() = 8;
                break;
                
            case eFormatCString:
                break;

            case eFormatInstruction:
                if (count_option_set)
                    byte_size_value = target->GetArchitecture().GetMaximumOpcodeByteSize();
                m_num_per_line = 1;
                break;

            case eFormatAddressInfo:
                if (!byte_size_option_set)
                    byte_size_value = target->GetArchitecture().GetAddressByteSize();
                m_num_per_line = 1;
                if (!count_option_set)
                    format_options.GetCountValue() = 8;
                break;

            case eFormatPointer:
                byte_size_value = target->GetArchitecture().GetAddressByteSize();
                if (!num_per_line_option_set)
                    m_num_per_line = 4;
                if (!count_option_set)
                    format_options.GetCountValue() = 8;
                break;
                
            case eFormatBinary:
            case eFormatFloat:
            case eFormatOctal:
            case eFormatDecimal:
            case eFormatEnum:
            case eFormatUnicode16:
            case eFormatUnicode32:
            case eFormatUnsigned:
            case eFormatHexFloat:
                if (!byte_size_option_set)
                    byte_size_value = 4;
                if (!num_per_line_option_set)
                    m_num_per_line = 1;
                if (!count_option_set)
                    format_options.GetCountValue() = 8;
                break;
            
            case eFormatBytes:
            case eFormatBytesWithASCII:
                if (byte_size_option_set)
                {
                    if (byte_size_value > 1)
                        error.SetErrorStringWithFormat ("display format (bytes/bytes with ascii) conflicts with the specified byte size %" PRIu64 "\n"
                                                        "\tconsider using a different display format or don't specify the byte size",
                                                        byte_size_value.GetCurrentValue());
                }
                else
                    byte_size_value = 1;
                if (!num_per_line_option_set)
                    m_num_per_line = 16;
                if (!count_option_set)
                    format_options.GetCountValue() = 32;
                break;

            case eFormatCharArray:
            case eFormatChar:
            case eFormatCharPrintable:
                if (!byte_size_option_set)
                    byte_size_value = 1;
                if (!num_per_line_option_set)
                    m_num_per_line = 32;
                if (!count_option_set)
                    format_options.GetCountValue() = 64;
                break;

            case eFormatComplex:
                if (!byte_size_option_set)
                    byte_size_value = 8;
                if (!num_per_line_option_set)
                    m_num_per_line = 1;
                if (!count_option_set)
                    format_options.GetCountValue() = 8;
                break;

            case eFormatComplexInteger:
                if (!byte_size_option_set)
                    byte_size_value = 8;
                if (!num_per_line_option_set)
                    m_num_per_line = 1;
                if (!count_option_set)
                    format_options.GetCountValue() = 8;
                break;

            case eFormatHex:
                if (!byte_size_option_set)
                    byte_size_value = 4;
                if (!num_per_line_option_set)
                {
                    switch (byte_size_value)
                    {
                        case 1:
                        case 2:
                            m_num_per_line = 8;
                            break;
                        case 4:
                            m_num_per_line = 4;
                            break;
                        case 8:
                            m_num_per_line = 2;
                            break;
                        default:
                            m_num_per_line = 1;
                            break;
                    }
                }
                if (!count_option_set)
                    count_value = 8;
                break;
                
            case eFormatVectorOfChar:
            case eFormatVectorOfSInt8:
            case eFormatVectorOfUInt8:
            case eFormatVectorOfSInt16:
            case eFormatVectorOfUInt16:
            case eFormatVectorOfSInt32:
            case eFormatVectorOfUInt32:
            case eFormatVectorOfSInt64:
            case eFormatVectorOfUInt64:
            case eFormatVectorOfFloat16:
            case eFormatVectorOfFloat32:
            case eFormatVectorOfFloat64:
            case eFormatVectorOfUInt128:
                if (!byte_size_option_set)
                    byte_size_value = 128;
                if (!num_per_line_option_set)
                    m_num_per_line = 1;
                if (!count_option_set)
                    count_value = 4;
                break;
        }
        return error;
    }

    bool
    AnyOptionWasSet () const
    {
        return m_num_per_line.OptionWasSet() ||
               m_output_as_binary ||
               m_view_as_type.OptionWasSet() ||
               m_offset.OptionWasSet();
    }
    
    OptionValueUInt64 m_num_per_line;
    bool m_output_as_binary;
    OptionValueString m_view_as_type;
    bool m_force;
    OptionValueUInt64 m_offset;
};

//----------------------------------------------------------------------
// Read memory from the inferior process
//----------------------------------------------------------------------
class CommandObjectMemoryRead : public CommandObjectParsed
{
public:
    CommandObjectMemoryRead (CommandInterpreter &interpreter) :
        CommandObjectParsed(interpreter,
                            "memory read",
                            "Read from the memory of the process being debugged.",
                            nullptr,
                            eCommandRequiresTarget | eCommandProcessMustBePaused),
        m_option_group (interpreter),
        m_format_options (eFormatBytesWithASCII, 1, 8),
        m_memory_options (),
        m_outfile_options (),
        m_varobj_options(),
        m_next_addr(LLDB_INVALID_ADDRESS),
        m_prev_byte_size(0),
        m_prev_format_options (eFormatBytesWithASCII, 1, 8),
        m_prev_memory_options (),
        m_prev_outfile_options (),
        m_prev_varobj_options()
    {
        CommandArgumentEntry arg1;
        CommandArgumentEntry arg2;
        CommandArgumentData start_addr_arg;
        CommandArgumentData end_addr_arg;
        
        // Define the first (and only) variant of this arg.
        start_addr_arg.arg_type = eArgTypeAddressOrExpression;
        start_addr_arg.arg_repetition = eArgRepeatPlain;
        
        // There is only one variant this argument could be; put it into the argument entry.
        arg1.push_back (start_addr_arg);
        
        // Define the first (and only) variant of this arg.
        end_addr_arg.arg_type = eArgTypeAddressOrExpression;
        end_addr_arg.arg_repetition = eArgRepeatOptional;
        
        // There is only one variant this argument could be; put it into the argument entry.
        arg2.push_back (end_addr_arg);
        
        // Push the data for the first argument into the m_arguments vector.
        m_arguments.push_back (arg1);
        m_arguments.push_back (arg2);
        
        // Add the "--format" and "--count" options to group 1 and 3
        m_option_group.Append (&m_format_options, 
                               OptionGroupFormat::OPTION_GROUP_FORMAT | OptionGroupFormat::OPTION_GROUP_COUNT, 
                               LLDB_OPT_SET_1 | LLDB_OPT_SET_2 | LLDB_OPT_SET_3);
        m_option_group.Append (&m_format_options, 
                               OptionGroupFormat::OPTION_GROUP_GDB_FMT, 
                               LLDB_OPT_SET_1 | LLDB_OPT_SET_3);
        // Add the "--size" option to group 1 and 2
        m_option_group.Append (&m_format_options, 
                               OptionGroupFormat::OPTION_GROUP_SIZE, 
                               LLDB_OPT_SET_1 | LLDB_OPT_SET_2);
        m_option_group.Append (&m_memory_options);
        m_option_group.Append (&m_outfile_options, LLDB_OPT_SET_ALL, LLDB_OPT_SET_1 | LLDB_OPT_SET_2 | LLDB_OPT_SET_3);
        m_option_group.Append (&m_varobj_options, LLDB_OPT_SET_ALL, LLDB_OPT_SET_3);
        m_option_group.Finalize();
    }

    ~CommandObjectMemoryRead() override = default;

    Options *
    GetOptions () override
    {
        return &m_option_group;
    }

    const char *
    GetRepeatCommand (Args &current_command_args, uint32_t index) override
    {
        return m_cmd_name.c_str();
    }

protected:
    bool
    DoExecute (Args& command, CommandReturnObject &result) override
    {
        // No need to check "target" for validity as eCommandRequiresTarget ensures it is valid
        Target *target = m_exe_ctx.GetTargetPtr();

        const size_t argc = command.GetArgumentCount();

        if ((argc == 0 && m_next_addr == LLDB_INVALID_ADDRESS) || argc > 2)
        {
            result.AppendErrorWithFormat ("%s takes a start address expression with an optional end address expression.\n", m_cmd_name.c_str());
            result.AppendRawWarning("Expressions should be quoted if they contain spaces or other special characters.\n");
            result.SetStatus(eReturnStatusFailed);
            return false;
        }

        CompilerType clang_ast_type;        
        Error error;

        const char *view_as_type_cstr = m_memory_options.m_view_as_type.GetCurrentValue();
        if (view_as_type_cstr && view_as_type_cstr[0])
        {
            // We are viewing memory as a type
            
            SymbolContext sc;
            const bool exact_match = false;
            TypeList type_list;
            uint32_t reference_count = 0;
            uint32_t pointer_count = 0;
            size_t idx;
            
#define ALL_KEYWORDS        \
    KEYWORD("const")        \
    KEYWORD("volatile")     \
    KEYWORD("restrict")     \
    KEYWORD("struct")       \
    KEYWORD("class")        \
    KEYWORD("union")
            
#define KEYWORD(s) s,
            static const char *g_keywords[] =
            {
                ALL_KEYWORDS
            };
#undef KEYWORD

#define KEYWORD(s) (sizeof(s) - 1),
            static const int g_keyword_lengths[] =
            {
                ALL_KEYWORDS
            };
#undef KEYWORD
            
#undef ALL_KEYWORDS
            
            static size_t g_num_keywords = sizeof(g_keywords) / sizeof(const char *);
            std::string type_str(view_as_type_cstr);
            
            // Remove all instances of g_keywords that are followed by spaces
            for (size_t i = 0; i < g_num_keywords; ++i)
            {
                const char *keyword = g_keywords[i];
                int keyword_len = g_keyword_lengths[i];
                
                idx = 0;
                while ((idx = type_str.find (keyword, idx)) != std::string::npos)
                {
                    if (type_str[idx + keyword_len] == ' ' || type_str[idx + keyword_len] == '\t')
                    {
                        type_str.erase(idx, keyword_len+1);
                        idx = 0;
                    }
                    else
                    {
                        idx += keyword_len;
                    }
                }
            }
            bool done = type_str.empty();
            // 
            idx = type_str.find_first_not_of (" \t");
            if (idx > 0 && idx != std::string::npos)
                type_str.erase (0, idx);
            while (!done)
            {
                // Strip trailing spaces
                if (type_str.empty())
                    done = true;
                else
                {
                    switch (type_str[type_str.size()-1])
                    {
                    case '*':
                        ++pointer_count;
                        LLVM_FALLTHROUGH;
                    case ' ':
                    case '\t':
                        type_str.erase(type_str.size()-1);
                        break;

                    case '&':
                        if (reference_count == 0)
                        {
                            reference_count = 1;
                            type_str.erase(type_str.size()-1);
                        }
                        else
                        {
                            result.AppendErrorWithFormat ("invalid type string: '%s'\n", view_as_type_cstr);
                            result.SetStatus(eReturnStatusFailed);
                            return false;
                        }
                        break;

                    default:
                        done = true;
                        break;
                    }
                }
            }
                    
            llvm::DenseSet<lldb_private::SymbolFile *> searched_symbol_files;
            ConstString lookup_type_name(type_str.c_str());
            StackFrame *frame = m_exe_ctx.GetFramePtr();
            if (frame)
            {
                sc = frame->GetSymbolContext (eSymbolContextModule);
                if (sc.module_sp)
                {
                    sc.module_sp->FindTypes (sc,
                                             lookup_type_name,
                                             exact_match,
                                             1,
                                             searched_symbol_files,
                                             type_list);
                }
            }
            if (type_list.GetSize() == 0)
            {
                target->GetImages().FindTypes (sc, 
                                               lookup_type_name, 
                                               exact_match, 
                                               1,
                                               searched_symbol_files,
                                               type_list);
            }

            if (type_list.GetSize() == 0 && lookup_type_name.GetCString() && *lookup_type_name.GetCString() == '$')
            {
                if (ClangPersistentVariables *persistent_vars = llvm::dyn_cast_or_null<ClangPersistentVariables>(target->GetPersistentExpressionStateForLanguage(lldb::eLanguageTypeC)))
                {
                    clang::TypeDecl *tdecl = persistent_vars->GetPersistentType(ConstString(lookup_type_name));

                    if (tdecl)
                    {
                        clang_ast_type.SetCompilerType(ClangASTContext::GetASTContext(&tdecl->getASTContext()),
                                                       reinterpret_cast<lldb::opaque_compiler_type_t>(const_cast<clang::Type*>(tdecl->getTypeForDecl())));
                    }
                }
            }

            if (!clang_ast_type.IsValid())
            {
                if (type_list.GetSize() == 0)
                {
                    result.AppendErrorWithFormat ("unable to find any types that match the raw type '%s' for full type '%s'\n",
                                                  lookup_type_name.GetCString(),
                                                  view_as_type_cstr);
                    result.SetStatus(eReturnStatusFailed);
                    return false;
                }
                else
                {
                    TypeSP type_sp (type_list.GetTypeAtIndex(0));
                    clang_ast_type = type_sp->GetFullCompilerType ();
                }
            }
            
            while (pointer_count > 0)
            {
                CompilerType pointer_type = clang_ast_type.GetPointerType();
                if (pointer_type.IsValid())
                    clang_ast_type = pointer_type;
                else
                {
                    result.AppendError ("unable make a pointer type\n");
                    result.SetStatus(eReturnStatusFailed);
                    return false;
                }
                --pointer_count;
            }

            m_format_options.GetByteSizeValue() = clang_ast_type.GetByteSize(nullptr);
            
            if (m_format_options.GetByteSizeValue() == 0)
            {
                result.AppendErrorWithFormat ("unable to get the byte size of the type '%s'\n", 
                                              view_as_type_cstr);
                result.SetStatus(eReturnStatusFailed);
                return false;
            }
            
            if (!m_format_options.GetCountValue().OptionWasSet())
                m_format_options.GetCountValue() = 1;
        }
        else
        {
            error = m_memory_options.FinalizeSettings (target, m_format_options);
        }

        // Look for invalid combinations of settings
        if (error.Fail())
        {
            result.AppendError(error.AsCString());
            result.SetStatus(eReturnStatusFailed);
            return false;
        }

        lldb::addr_t addr;
        size_t total_byte_size = 0;
        if (argc == 0)
        {
            // Use the last address and byte size and all options as they were
            // if no options have been set
            addr = m_next_addr;
            total_byte_size = m_prev_byte_size;
            clang_ast_type = m_prev_clang_ast_type;
            if (!m_format_options.AnyOptionWasSet() &&
                !m_memory_options.AnyOptionWasSet() &&
                !m_outfile_options.AnyOptionWasSet() &&
                !m_varobj_options.AnyOptionWasSet())
            {
                m_format_options = m_prev_format_options;
                m_memory_options = m_prev_memory_options;
                m_outfile_options = m_prev_outfile_options;
                m_varobj_options = m_prev_varobj_options;
            }
        }

        size_t item_count = m_format_options.GetCountValue().GetCurrentValue();

        // TODO For non-8-bit byte addressable architectures this needs to be
        // revisited to fully support all lldb's range of formatting options.
        // Furthermore code memory reads (for those architectures) will not
        // be correctly formatted even w/o formatting options.
        size_t item_byte_size =
            target->GetArchitecture().GetDataByteSize() > 1 ?
            target->GetArchitecture().GetDataByteSize() :
            m_format_options.GetByteSizeValue().GetCurrentValue();

        const size_t num_per_line = m_memory_options.m_num_per_line.GetCurrentValue();

        if (total_byte_size == 0)
        {
            total_byte_size = item_count * item_byte_size;
            if (total_byte_size == 0)
                total_byte_size = 32;
        }

        if (argc > 0)
            addr = Args::StringToAddress(&m_exe_ctx, command.GetArgumentAtIndex(0), LLDB_INVALID_ADDRESS, &error);

        if (addr == LLDB_INVALID_ADDRESS)
        {
            result.AppendError("invalid start address expression.");
            result.AppendError(error.AsCString());
            result.SetStatus(eReturnStatusFailed);
            return false;
        }

        if (argc == 2)
        {
            lldb::addr_t end_addr = Args::StringToAddress(&m_exe_ctx, command.GetArgumentAtIndex(1),
                                                          LLDB_INVALID_ADDRESS, nullptr);
            if (end_addr == LLDB_INVALID_ADDRESS)
            {
                result.AppendError("invalid end address expression.");
                result.AppendError(error.AsCString());
                result.SetStatus(eReturnStatusFailed);
                return false;
            }
            else if (end_addr <= addr)
            {
                result.AppendErrorWithFormat("end address (0x%" PRIx64 ") must be greater that the start address (0x%" PRIx64 ").\n", end_addr, addr);
                result.SetStatus(eReturnStatusFailed);
                return false;
            }
            else if (m_format_options.GetCountValue().OptionWasSet())
            {
                result.AppendErrorWithFormat("specify either the end address (0x%" PRIx64 ") or the count (--count %" PRIu64 "), not both.\n", end_addr, (uint64_t)item_count);
                result.SetStatus(eReturnStatusFailed);
                return false;
            }

            total_byte_size = end_addr - addr;
            item_count = total_byte_size / item_byte_size;
        }

        uint32_t max_unforced_size = target->GetMaximumMemReadSize();
        
        if (total_byte_size > max_unforced_size && !m_memory_options.m_force)
        {
            result.AppendErrorWithFormat("Normally, \'memory read\' will not read over %" PRIu32 " bytes of data.\n",max_unforced_size);
            result.AppendErrorWithFormat("Please use --force to override this restriction just once.\n");
            result.AppendErrorWithFormat("or set target.max-memory-read-size if you will often need a larger limit.\n");
            return false;
        }
        
        DataBufferSP data_sp;
        size_t bytes_read = 0;
        if (clang_ast_type.GetOpaqueQualType())
        {
            // Make sure we don't display our type as ASCII bytes like the default memory read
            if (!m_format_options.GetFormatValue().OptionWasSet())
                m_format_options.GetFormatValue().SetCurrentValue(eFormatDefault);

            bytes_read = clang_ast_type.GetByteSize(nullptr) * m_format_options.GetCountValue().GetCurrentValue();
                
            if (argc > 0)
                addr = addr + (clang_ast_type.GetByteSize(nullptr) * m_memory_options.m_offset.GetCurrentValue());
        }
        else if (m_format_options.GetFormatValue().GetCurrentValue() != eFormatCString)
        {
            data_sp.reset (new DataBufferHeap (total_byte_size, '\0'));
            if (data_sp->GetBytes() == nullptr)
            {
                result.AppendErrorWithFormat ("can't allocate 0x%" PRIx32 " bytes for the memory read buffer, specify a smaller size to read", (uint32_t)total_byte_size);
                result.SetStatus(eReturnStatusFailed);
                return false;
            }

            Address address(addr, nullptr);
            bytes_read = target->ReadMemory(address, false, data_sp->GetBytes (), data_sp->GetByteSize(), error);
            if (bytes_read == 0)
            {
                const char *error_cstr = error.AsCString();
                if (error_cstr && error_cstr[0])
                {
                    result.AppendError(error_cstr);
                }
                else
                {
                    result.AppendErrorWithFormat("failed to read memory from 0x%" PRIx64 ".\n", addr);
                }
                result.SetStatus(eReturnStatusFailed);
                return false;
            }
            
            if (bytes_read < total_byte_size)
                result.AppendWarningWithFormat("Not all bytes (%" PRIu64 "/%" PRIu64 ") were able to be read from 0x%" PRIx64 ".\n", (uint64_t)bytes_read, (uint64_t)total_byte_size, addr);
        }
        else
        {
            // we treat c-strings as a special case because they do not have a fixed size
            if (m_format_options.GetByteSizeValue().OptionWasSet() && !m_format_options.HasGDBFormat())
                item_byte_size = m_format_options.GetByteSizeValue().GetCurrentValue();
            else
                item_byte_size = target->GetMaximumSizeOfStringSummary();
            if (!m_format_options.GetCountValue().OptionWasSet())
                item_count = 1;
            data_sp.reset (new DataBufferHeap ((item_byte_size+1) * item_count, '\0')); // account for NULLs as necessary
            if (data_sp->GetBytes() == nullptr)
            {
                result.AppendErrorWithFormat ("can't allocate 0x%" PRIx64 " bytes for the memory read buffer, specify a smaller size to read", (uint64_t)((item_byte_size+1) * item_count));
                result.SetStatus(eReturnStatusFailed);
                return false;
            }
            uint8_t *data_ptr = data_sp->GetBytes();
            auto data_addr = addr;
            auto count = item_count;
            item_count = 0;
            bool break_on_no_NULL = false;
            while (item_count < count)
            {
                std::string buffer;
                buffer.resize(item_byte_size+1,0);
                Error error;
                size_t read = target->ReadCStringFromMemory(data_addr, &buffer[0], item_byte_size+1, error);
                if (error.Fail())
                {
                    result.AppendErrorWithFormat("failed to read memory from 0x%" PRIx64 ".\n", addr);
                    result.SetStatus(eReturnStatusFailed);
                    return false;
                }
                
                if (item_byte_size == read)
                {
                    result.AppendWarningWithFormat("unable to find a NULL terminated string at 0x%" PRIx64 ".Consider increasing the maximum read length.\n", data_addr);
                    --read;
                    break_on_no_NULL = true;
                }
                else
                    ++read; // account for final NULL byte
                
                memcpy(data_ptr, &buffer[0], read);
                data_ptr += read;
                data_addr += read;
                bytes_read += read;
                item_count++; // if we break early we know we only read item_count strings
                
                if (break_on_no_NULL)
                    break;
            }
            data_sp.reset(new DataBufferHeap(data_sp->GetBytes(),bytes_read+1));
        }

        m_next_addr = addr + bytes_read;
        m_prev_byte_size = bytes_read;
        m_prev_format_options = m_format_options;
        m_prev_memory_options = m_memory_options;
        m_prev_outfile_options = m_outfile_options;
        m_prev_varobj_options = m_varobj_options;
        m_prev_clang_ast_type = clang_ast_type;

        StreamFile outfile_stream;
        Stream *output_stream = nullptr;
        const FileSpec &outfile_spec = m_outfile_options.GetFile().GetCurrentValue();
        if (outfile_spec)
        {
            char path[PATH_MAX];
            outfile_spec.GetPath (path, sizeof(path));
            
            uint32_t open_options = File::eOpenOptionWrite | File::eOpenOptionCanCreate;
            const bool append = m_outfile_options.GetAppend().GetCurrentValue();
            if (append)
                open_options |= File::eOpenOptionAppend;
            
            if (outfile_stream.GetFile ().Open (path, open_options).Success())
            {
                if (m_memory_options.m_output_as_binary)
                {
                    const size_t bytes_written = outfile_stream.Write (data_sp->GetBytes(), bytes_read);
                    if (bytes_written > 0)
                    {
                        result.GetOutputStream().Printf ("%zi bytes %s to '%s'\n", 
                                                         bytes_written, 
                                                         append ? "appended" : "written", 
                                                         path);
                        return true;
                    }
                    else 
                    {
                        result.AppendErrorWithFormat("Failed to write %" PRIu64 " bytes to '%s'.\n", (uint64_t)bytes_read, path);
                        result.SetStatus(eReturnStatusFailed);
                        return false;
                    }
                }
                else
                {
                    // We are going to write ASCII to the file just point the
                    // output_stream to our outfile_stream...
                    output_stream = &outfile_stream;
                }
            }
            else 
            {
                result.AppendErrorWithFormat("Failed to open file '%s' for %s.\n", path, append ? "append" : "write");
                result.SetStatus(eReturnStatusFailed);
                return false;
            }
        }
        else 
        {
            output_stream = &result.GetOutputStream();
        }

        ExecutionContextScope *exe_scope = m_exe_ctx.GetBestExecutionContextScope();
        if (clang_ast_type.GetOpaqueQualType())
        {
            for (uint32_t i = 0; i<item_count; ++i)
            {
                addr_t item_addr = addr + (i * item_byte_size);
                Address address (item_addr);
                StreamString name_strm;
                name_strm.Printf ("0x%" PRIx64, item_addr);
                ValueObjectSP valobj_sp (ValueObjectMemory::Create (exe_scope, 
                                                                    name_strm.GetString().c_str(), 
                                                                    address, 
                                                                    clang_ast_type));
                if (valobj_sp)
                {
                    Format format = m_format_options.GetFormat();
                    if (format != eFormatDefault)
                        valobj_sp->SetFormat (format);

                    DumpValueObjectOptions options(m_varobj_options.GetAsDumpOptions(eLanguageRuntimeDescriptionDisplayVerbosityFull,format));
                    
                    valobj_sp->Dump(*output_stream,options);
                }
                else
                {
                    result.AppendErrorWithFormat ("failed to create a value object for: (%s) %s\n", 
                                                  view_as_type_cstr, 
                                                  name_strm.GetString().c_str());
                    result.SetStatus(eReturnStatusFailed);
                    return false;
                }
            }
            return true;
        }

        result.SetStatus(eReturnStatusSuccessFinishResult);
        DataExtractor data (data_sp, 
                            target->GetArchitecture().GetByteOrder(), 
                            target->GetArchitecture().GetAddressByteSize(),
                            target->GetArchitecture().GetDataByteSize());
        
        Format format = m_format_options.GetFormat();
        if ( ( (format == eFormatChar) || (format == eFormatCharPrintable) )
            && (item_byte_size != 1))
        {
            // if a count was not passed, or it is 1
            if (!m_format_options.GetCountValue().OptionWasSet() || item_count == 1)
            {
                // this turns requests such as
                // memory read -fc -s10 -c1 *charPtrPtr
                // which make no sense (what is a char of size 10?)
                // into a request for fetching 10 chars of size 1 from the same memory location
                format = eFormatCharArray;
                item_count = item_byte_size;
                item_byte_size = 1;
            }
            else
            {
                // here we passed a count, and it was not 1
                // so we have a byte_size and a count
                // we could well multiply those, but instead let's just fail
                result.AppendErrorWithFormat("reading memory as characters of size %" PRIu64 " is not supported", (uint64_t)item_byte_size);
                result.SetStatus(eReturnStatusFailed);
                return false;
            }
        }

        assert (output_stream);
        size_t bytes_dumped = data.Dump (output_stream,
                                         0,
                                         format,
                                         item_byte_size,
                                         item_count,
                                         num_per_line / target->GetArchitecture().GetDataByteSize(),
                                         addr,
                                         0,
                                         0,
                                         exe_scope);
        m_next_addr = addr + bytes_dumped;
        output_stream->EOL();
        return true;
    }

    OptionGroupOptions m_option_group;
    OptionGroupFormat m_format_options;
    OptionGroupReadMemory m_memory_options;
    OptionGroupOutputFile m_outfile_options;
    OptionGroupValueObjectDisplay m_varobj_options;
    lldb::addr_t m_next_addr;
    lldb::addr_t m_prev_byte_size; 
    OptionGroupFormat m_prev_format_options;
    OptionGroupReadMemory m_prev_memory_options;
    OptionGroupOutputFile m_prev_outfile_options;
    OptionGroupValueObjectDisplay m_prev_varobj_options;
    CompilerType m_prev_clang_ast_type;
};

OptionDefinition
g_memory_find_option_table[] =
{
    { LLDB_OPT_SET_1, false, "expression", 'e', OptionParser::eRequiredArgument, nullptr, nullptr, 0, eArgTypeExpression, "Evaluate an expression to obtain a byte pattern."},
    { LLDB_OPT_SET_2, false, "string", 's', OptionParser::eRequiredArgument, nullptr, nullptr, 0, eArgTypeName,   "Use text to find a byte pattern."},
    { LLDB_OPT_SET_1|LLDB_OPT_SET_2, false, "count", 'c', OptionParser::eRequiredArgument, nullptr, nullptr, 0, eArgTypeCount,   "How many times to perform the search."},
    { LLDB_OPT_SET_1|LLDB_OPT_SET_2, false, "dump-offset", 'o', OptionParser::eRequiredArgument, nullptr, nullptr, 0, eArgTypeOffset,   "When dumping memory for a match, an offset from the match location to start dumping from."},
};

//----------------------------------------------------------------------
// Find the specified data in memory
//----------------------------------------------------------------------
class CommandObjectMemoryFind : public CommandObjectParsed
{
public:
  class OptionGroupFindMemory : public OptionGroup
  {
  public:
    OptionGroupFindMemory () :
      OptionGroup(),
      m_count(1),
      m_offset(0)
    {
    }

    ~OptionGroupFindMemory() override = default;

    uint32_t
    GetNumDefinitions () override
    {
      return sizeof (g_memory_find_option_table) / sizeof (OptionDefinition);
    }
    
    const OptionDefinition*
    GetDefinitions () override
    {
      return g_memory_find_option_table;
    }
    
    Error
    SetOptionValue (CommandInterpreter &interpreter,
                    uint32_t option_idx,
                    const char *option_arg) override
    {
        Error error;
        const int short_option = g_memory_find_option_table[option_idx].short_option;
      
        switch (short_option)
        {
        case 'e':
              m_expr.SetValueFromString(option_arg);
              break;
          
        case 's':
              m_string.SetValueFromString(option_arg);
              break;
          
        case 'c':
              if (m_count.SetValueFromString(option_arg).Fail())
                  error.SetErrorString("unrecognized value for count");
              break;
                
        case 'o':
               if (m_offset.SetValueFromString(option_arg).Fail())
                   error.SetErrorString("unrecognized value for dump-offset");
                break;

        default:
              error.SetErrorStringWithFormat("unrecognized short option '%c'", short_option);
              break;
        }
        return error;
    }
    
    void
    OptionParsingStarting (CommandInterpreter &interpreter) override
    {
        m_expr.Clear();
        m_string.Clear();
        m_count.Clear();
    }
    
      OptionValueString m_expr;
      OptionValueString m_string;
      OptionValueUInt64 m_count;
      OptionValueUInt64 m_offset;
  };
  
  CommandObjectMemoryFind (CommandInterpreter &interpreter) :
  CommandObjectParsed(interpreter,
                      "memory find",
                      "Find a value in the memory of the process being debugged.",
                      nullptr,
                      eCommandRequiresProcess | eCommandProcessMustBeLaunched),
  m_option_group (interpreter),
  m_memory_options ()
  {
    CommandArgumentEntry arg1;
    CommandArgumentEntry arg2;
    CommandArgumentData addr_arg;
    CommandArgumentData value_arg;
    
    // Define the first (and only) variant of this arg.
    addr_arg.arg_type = eArgTypeAddressOrExpression;
    addr_arg.arg_repetition = eArgRepeatPlain;
    
    // There is only one variant this argument could be; put it into the argument entry.
    arg1.push_back (addr_arg);
    
    // Define the first (and only) variant of this arg.
    value_arg.arg_type = eArgTypeAddressOrExpression;
    value_arg.arg_repetition = eArgRepeatPlain;
    
    // There is only one variant this argument could be; put it into the argument entry.
    arg2.push_back (value_arg);
    
    // Push the data for the first argument into the m_arguments vector.
    m_arguments.push_back (arg1);
    m_arguments.push_back (arg2);
    
    m_option_group.Append (&m_memory_options, LLDB_OPT_SET_ALL, LLDB_OPT_SET_2);
    m_option_group.Finalize();
  }

  ~CommandObjectMemoryFind() override = default;

  Options *
  GetOptions () override
  {
    return &m_option_group;
  }
  
protected:
  bool
  DoExecute (Args& command, CommandReturnObject &result) override
  {
      // No need to check "process" for validity as eCommandRequiresProcess ensures it is valid
      Process *process = m_exe_ctx.GetProcessPtr();

      const size_t argc = command.GetArgumentCount();

      if (argc != 2)
      {
          result.AppendError("two addresses needed for memory find");
          return false;
      }

      Error error;
      lldb::addr_t low_addr = Args::StringToAddress(&m_exe_ctx, command.GetArgumentAtIndex(0),LLDB_INVALID_ADDRESS,&error);
      if (low_addr == LLDB_INVALID_ADDRESS || error.Fail())
      {
          result.AppendError("invalid low address");
          return false;
      }
      lldb::addr_t high_addr = Args::StringToAddress(&m_exe_ctx, command.GetArgumentAtIndex(1),LLDB_INVALID_ADDRESS,&error);
      if (high_addr == LLDB_INVALID_ADDRESS || error.Fail())
      {
          result.AppendError("invalid high address");
          return false;
      }

      if (high_addr <= low_addr)
      {
          result.AppendError("starting address must be smaller than ending address");
          return false;
      }
      
      lldb::addr_t found_location = LLDB_INVALID_ADDRESS;
      
      DataBufferHeap buffer;
      
      if (m_memory_options.m_string.OptionWasSet())
          buffer.CopyData(m_memory_options.m_string.GetStringValue(), strlen(m_memory_options.m_string.GetStringValue()));
      else if (m_memory_options.m_expr.OptionWasSet())
      {
          StackFrame* frame = m_exe_ctx.GetFramePtr();
          ValueObjectSP result_sp;
          if ((eExpressionCompleted == process->GetTarget().EvaluateExpression(m_memory_options.m_expr.GetStringValue(), frame, result_sp)) &&
              result_sp)
          {
              uint64_t value = result_sp->GetValueAsUnsigned(0);
              switch (result_sp->GetCompilerType().GetByteSize(nullptr))
              {
                  case 1: {
                      uint8_t byte = (uint8_t)value;
                      buffer.CopyData(&byte,1);
                  }
                      break;
                  case 2: {
                      uint16_t word = (uint16_t)value;
                      buffer.CopyData(&word,2);
                  }
                      break;
                  case 4: {
                      uint32_t lword = (uint32_t)value;
                      buffer.CopyData(&lword,4);
                  }
                      break;
                  case 8: {
                      buffer.CopyData(&value, 8);
                  }
                      break;
                  case 3:
                  case 5:
                  case 6:
                  case 7:
                      result.AppendError("unknown type. pass a string instead");
                      return false;
                  default:
                      result.AppendError("result size larger than 8 bytes. pass a string instead");
                      return false;
              }
          }
          else
          {
              result.AppendError("expression evaluation failed. pass a string instead");
              return false;
          }
      }
      else
      {
          result.AppendError("please pass either a block of text, or an expression to evaluate.");
          return false;
      }
      
      size_t count = m_memory_options.m_count.GetCurrentValue();
      found_location = low_addr;
      bool ever_found = false;
      while (count)
      {
          found_location = Search(found_location, high_addr, buffer.GetBytes(), buffer.GetByteSize());
          if (found_location == LLDB_INVALID_ADDRESS)
          {
              if (!ever_found)
              {
                  result.AppendMessage("data not found within the range.\n");
                  result.SetStatus(lldb::eReturnStatusSuccessFinishNoResult);
              }
              else
                  result.AppendMessage("no more matches within the range.\n");
              break;
          }
          result.AppendMessageWithFormat("data found at location: 0x%" PRIx64 "\n", found_location);

          DataBufferHeap dumpbuffer(32,0);
          process->ReadMemory(found_location+m_memory_options.m_offset.GetCurrentValue(), dumpbuffer.GetBytes(), dumpbuffer.GetByteSize(), error);
          if (!error.Fail())
          {
              DataExtractor data(dumpbuffer.GetBytes(), dumpbuffer.GetByteSize(), process->GetByteOrder(), process->GetAddressByteSize());
              data.Dump(&result.GetOutputStream(), 0, lldb::eFormatBytesWithASCII, 1, dumpbuffer.GetByteSize(), 16, found_location+m_memory_options.m_offset.GetCurrentValue(), 0, 0);
              result.GetOutputStream().EOL();
          }

          --count;
          found_location++;
          ever_found = true;
      }
      
      result.SetStatus(lldb::eReturnStatusSuccessFinishResult);
      return true;
  }
    
    lldb::addr_t
    Search (lldb::addr_t low,
            lldb::addr_t high,
            uint8_t* buffer,
            size_t buffer_size)
    {
        Process *process = m_exe_ctx.GetProcessPtr();
        DataBufferHeap heap(buffer_size, 0);
        for (auto ptr = low;
             ptr < high;
             ptr++)
        {
            Error error;
            process->ReadMemory(ptr, heap.GetBytes(), buffer_size, error);
            if (error.Fail())
                return LLDB_INVALID_ADDRESS;
            if (memcmp(heap.GetBytes(), buffer, buffer_size) == 0)
                return ptr;
        }
        return LLDB_INVALID_ADDRESS;
    }
  
    OptionGroupOptions m_option_group;
    OptionGroupFindMemory m_memory_options;
};

OptionDefinition
g_memory_write_option_table[] =
{
{ LLDB_OPT_SET_1, true,  "infile", 'i', OptionParser::eRequiredArgument, nullptr, nullptr, 0, eArgTypeFilename, "Write memory using the contents of a file."},
{ LLDB_OPT_SET_1, false, "offset", 'o', OptionParser::eRequiredArgument, nullptr, nullptr, 0, eArgTypeOffset,   "Start writing bytes from an offset within the input file."},
};

//----------------------------------------------------------------------
// Write memory to the inferior process
//----------------------------------------------------------------------
class CommandObjectMemoryWrite : public CommandObjectParsed
{
public:
    class OptionGroupWriteMemory : public OptionGroup
    {
    public:
        OptionGroupWriteMemory () :
            OptionGroup()
        {
        }

        ~OptionGroupWriteMemory() override = default;

        uint32_t
        GetNumDefinitions () override
        {
            return sizeof (g_memory_write_option_table) / sizeof (OptionDefinition);
        }
      
        const OptionDefinition*
        GetDefinitions () override
        {
            return g_memory_write_option_table;
        }
      
        Error
        SetOptionValue (CommandInterpreter &interpreter,
                        uint32_t option_idx,
                        const char *option_arg) override
        {
            Error error;
            const int short_option = g_memory_write_option_table[option_idx].short_option;
          
            switch (short_option)
            {
                case 'i':
                    m_infile.SetFile (option_arg, true);
                    if (!m_infile.Exists())
                    {
                        m_infile.Clear();
                        error.SetErrorStringWithFormat("input file does not exist: '%s'", option_arg);
                    }
                    break;
                
                case 'o':
                    {
                        bool success;
                        m_infile_offset = StringConvert::ToUInt64(option_arg, 0, 0, &success);
                        if (!success)
                        {
                            error.SetErrorStringWithFormat("invalid offset string '%s'", option_arg);
                        }
                    }
                    break;
                    
                default:
                    error.SetErrorStringWithFormat("unrecognized short option '%c'", short_option);
                    break;
            }
            return error;
        }
        
        void
        OptionParsingStarting (CommandInterpreter &interpreter) override
        {
            m_infile.Clear();
            m_infile_offset = 0;
        }

        FileSpec m_infile;
        off_t m_infile_offset;
    };

    CommandObjectMemoryWrite (CommandInterpreter &interpreter) :
        CommandObjectParsed(interpreter,
                            "memory write",
                            "Write to the memory of the process being debugged.",
                            nullptr,
                            eCommandRequiresProcess | eCommandProcessMustBeLaunched),
        m_option_group (interpreter),
        m_format_options (eFormatBytes, 1, UINT64_MAX),
        m_memory_options ()
    {
        CommandArgumentEntry arg1;
        CommandArgumentEntry arg2;
        CommandArgumentData addr_arg;
        CommandArgumentData value_arg;
        
        // Define the first (and only) variant of this arg.
        addr_arg.arg_type = eArgTypeAddress;
        addr_arg.arg_repetition = eArgRepeatPlain;
        
        // There is only one variant this argument could be; put it into the argument entry.
        arg1.push_back (addr_arg);
        
        // Define the first (and only) variant of this arg.
        value_arg.arg_type = eArgTypeValue;
        value_arg.arg_repetition = eArgRepeatPlus;
        
        // There is only one variant this argument could be; put it into the argument entry.
        arg2.push_back (value_arg);
        
        // Push the data for the first argument into the m_arguments vector.
        m_arguments.push_back (arg1);
        m_arguments.push_back (arg2);
        
        m_option_group.Append (&m_format_options, OptionGroupFormat::OPTION_GROUP_FORMAT, LLDB_OPT_SET_1);
        m_option_group.Append (&m_format_options, OptionGroupFormat::OPTION_GROUP_SIZE  , LLDB_OPT_SET_1|LLDB_OPT_SET_2);
        m_option_group.Append (&m_memory_options, LLDB_OPT_SET_ALL, LLDB_OPT_SET_2);
        m_option_group.Finalize();
    }

    ~CommandObjectMemoryWrite() override = default;

    Options *
    GetOptions () override
    {
        return &m_option_group;
    }

    bool
    UIntValueIsValidForSize (uint64_t uval64, size_t total_byte_size)
    {
        if (total_byte_size > 8)
            return false;

        if (total_byte_size == 8)
            return true;

        const uint64_t max = ((uint64_t)1 << (uint64_t)(total_byte_size * 8)) - 1;
        return uval64 <= max;
    }

    bool
    SIntValueIsValidForSize (int64_t sval64, size_t total_byte_size)
    {
        if (total_byte_size > 8)
            return false;

        if (total_byte_size == 8)
            return true;

        const int64_t max = ((int64_t)1 << (uint64_t)(total_byte_size * 8 - 1)) - 1;
        const int64_t min = ~(max);
        return min <= sval64 && sval64 <= max;
    }

protected:
    bool
    DoExecute (Args& command, CommandReturnObject &result) override
    {
        // No need to check "process" for validity as eCommandRequiresProcess ensures it is valid
        Process *process = m_exe_ctx.GetProcessPtr();

        const size_t argc = command.GetArgumentCount();

        if (m_memory_options.m_infile)
        {
            if (argc < 1)
            {
                result.AppendErrorWithFormat ("%s takes a destination address when writing file contents.\n", m_cmd_name.c_str());
                result.SetStatus(eReturnStatusFailed);
                return false;
            }       
        }
        else if (argc < 2)
        {
            result.AppendErrorWithFormat ("%s takes a destination address and at least one value.\n", m_cmd_name.c_str());
            result.SetStatus(eReturnStatusFailed);
            return false;
        }

        StreamString buffer (Stream::eBinary,
                             process->GetTarget().GetArchitecture().GetAddressByteSize(),
                             process->GetTarget().GetArchitecture().GetByteOrder());

        OptionValueUInt64 &byte_size_value = m_format_options.GetByteSizeValue();
        size_t item_byte_size = byte_size_value.GetCurrentValue();

        Error error;
        lldb::addr_t addr = Args::StringToAddress (&m_exe_ctx,
                                                   command.GetArgumentAtIndex(0),
                                                   LLDB_INVALID_ADDRESS,
                                                   &error);

        if (addr == LLDB_INVALID_ADDRESS)
        {
            result.AppendError("invalid address expression\n");
            result.AppendError(error.AsCString());
            result.SetStatus(eReturnStatusFailed);
            return false;
        }
        
        if (m_memory_options.m_infile)
        {
            size_t length = SIZE_MAX;
            if (item_byte_size > 1)
                length = item_byte_size;
            lldb::DataBufferSP data_sp (m_memory_options.m_infile.ReadFileContents (m_memory_options.m_infile_offset, length));
            if (data_sp)
            {
                length = data_sp->GetByteSize();
                if (length > 0)
                {
                    Error error;
                    size_t bytes_written = process->WriteMemory (addr, data_sp->GetBytes(), length, error);
                    
                    if (bytes_written == length)
                    {
                        // All bytes written
                        result.GetOutputStream().Printf("%" PRIu64 " bytes were written to 0x%" PRIx64 "\n", (uint64_t)bytes_written, addr);
                        result.SetStatus(eReturnStatusSuccessFinishResult);
                    }
                    else if (bytes_written > 0)
                    {
                        // Some byte written
                        result.GetOutputStream().Printf("%" PRIu64 " bytes of %" PRIu64 " requested were written to 0x%" PRIx64 "\n", (uint64_t)bytes_written, (uint64_t)length, addr);
                        result.SetStatus(eReturnStatusSuccessFinishResult);
                    }
                    else 
                    {
                        result.AppendErrorWithFormat ("Memory write to 0x%" PRIx64 " failed: %s.\n", addr, error.AsCString());
                        result.SetStatus(eReturnStatusFailed);
                    }
                }
            }
            else
            {
                result.AppendErrorWithFormat ("Unable to read contents of file.\n");
                result.SetStatus(eReturnStatusFailed);
            }
            return result.Succeeded();
        }
        else if (item_byte_size == 0)
        {
            if (m_format_options.GetFormat() == eFormatPointer)
                item_byte_size = buffer.GetAddressByteSize();
            else
                item_byte_size = 1;
        }

        command.Shift(); // shift off the address argument
        uint64_t uval64;
        int64_t sval64;
        bool success = false;
        const size_t num_value_args = command.GetArgumentCount();
        for (size_t i=0; i<num_value_args; ++i)
        {
            const char *value_str = command.GetArgumentAtIndex(i);

            switch (m_format_options.GetFormat())
            {
            case kNumFormats:
            case eFormatFloat:  // TODO: add support for floats soon
            case eFormatCharPrintable:
            case eFormatBytesWithASCII:
            case eFormatComplex:
            case eFormatEnum:
            case eFormatUnicode16:
            case eFormatUnicode32:
            case eFormatVectorOfChar:
            case eFormatVectorOfSInt8:
            case eFormatVectorOfUInt8:
            case eFormatVectorOfSInt16:
            case eFormatVectorOfUInt16:
            case eFormatVectorOfSInt32:
            case eFormatVectorOfUInt32:
            case eFormatVectorOfSInt64:
            case eFormatVectorOfUInt64:
            case eFormatVectorOfFloat16:
            case eFormatVectorOfFloat32:
            case eFormatVectorOfFloat64:
            case eFormatVectorOfUInt128:
            case eFormatOSType:
            case eFormatComplexInteger:
            case eFormatAddressInfo:
            case eFormatHexFloat:
            case eFormatInstruction:
            case eFormatVoid:
                result.AppendError("unsupported format for writing memory");
                result.SetStatus(eReturnStatusFailed);
                return false;

            case eFormatDefault:
            case eFormatBytes:
            case eFormatHex:
            case eFormatHexUppercase:
            case eFormatPointer:
                // Decode hex bytes
                uval64 = StringConvert::ToUInt64(value_str, UINT64_MAX, 16, &success);
                if (!success)
                {
                    result.AppendErrorWithFormat ("'%s' is not a valid hex string value.\n", value_str);
                    result.SetStatus(eReturnStatusFailed);
                    return false;
                }
                else if (!UIntValueIsValidForSize (uval64, item_byte_size))
                {
                    result.AppendErrorWithFormat("Value 0x%" PRIx64 " is too large to fit in a %" PRIu64 " byte unsigned integer value.\n", uval64, (uint64_t)item_byte_size);
                    result.SetStatus(eReturnStatusFailed);
                    return false;
                }
                buffer.PutMaxHex64 (uval64, item_byte_size);
                break;

            case eFormatBoolean:
                uval64 = Args::StringToBoolean(value_str, false, &success);
                if (!success)
                {
                    result.AppendErrorWithFormat ("'%s' is not a valid boolean string value.\n", value_str);
                    result.SetStatus(eReturnStatusFailed);
                    return false;
                }
                buffer.PutMaxHex64 (uval64, item_byte_size);
                break;

            case eFormatBinary:
                uval64 = StringConvert::ToUInt64(value_str, UINT64_MAX, 2, &success);
                if (!success)
                {
                    result.AppendErrorWithFormat ("'%s' is not a valid binary string value.\n", value_str);
                    result.SetStatus(eReturnStatusFailed);
                    return false;
                }
                else if (!UIntValueIsValidForSize (uval64, item_byte_size))
                {
                    result.AppendErrorWithFormat("Value 0x%" PRIx64 " is too large to fit in a %" PRIu64 " byte unsigned integer value.\n", uval64, (uint64_t)item_byte_size);
                    result.SetStatus(eReturnStatusFailed);
                    return false;
                }
                buffer.PutMaxHex64 (uval64, item_byte_size);
                break;

            case eFormatCharArray:
            case eFormatChar:
            case eFormatCString:
                if (value_str[0])
                {
                    size_t len = strlen (value_str);
                    // Include the NULL for C strings...
                    if (m_format_options.GetFormat() == eFormatCString)
                        ++len;
                    Error error;
                    if (process->WriteMemory (addr, value_str, len, error) == len)
                    {
                        addr += len;
                    }
                    else
                    {
                        result.AppendErrorWithFormat ("Memory write to 0x%" PRIx64 " failed: %s.\n", addr, error.AsCString());
                        result.SetStatus(eReturnStatusFailed);
                        return false;
                    }
                }
                break;

            case eFormatDecimal:
                sval64 = StringConvert::ToSInt64(value_str, INT64_MAX, 0, &success);
                if (!success)
                {
                    result.AppendErrorWithFormat ("'%s' is not a valid signed decimal value.\n", value_str);
                    result.SetStatus(eReturnStatusFailed);
                    return false;
                }
                else if (!SIntValueIsValidForSize (sval64, item_byte_size))
                {
                    result.AppendErrorWithFormat ("Value %" PRIi64 " is too large or small to fit in a %" PRIu64 " byte signed integer value.\n", sval64, (uint64_t)item_byte_size);
                    result.SetStatus(eReturnStatusFailed);
                    return false;
                }
                buffer.PutMaxHex64 (sval64, item_byte_size);
                break;

            case eFormatUnsigned:
                uval64 = StringConvert::ToUInt64(value_str, UINT64_MAX, 0, &success);
                if (!success)
                {
                    result.AppendErrorWithFormat ("'%s' is not a valid unsigned decimal string value.\n", value_str);
                    result.SetStatus(eReturnStatusFailed);
                    return false;
                }
                else if (!UIntValueIsValidForSize (uval64, item_byte_size))
                {
                    result.AppendErrorWithFormat ("Value %" PRIu64 " is too large to fit in a %" PRIu64 " byte unsigned integer value.\n", uval64, (uint64_t)item_byte_size);
                    result.SetStatus(eReturnStatusFailed);
                    return false;
                }
                buffer.PutMaxHex64 (uval64, item_byte_size);
                break;

            case eFormatOctal:
                uval64 = StringConvert::ToUInt64(value_str, UINT64_MAX, 8, &success);
                if (!success)
                {
                    result.AppendErrorWithFormat ("'%s' is not a valid octal string value.\n", value_str);
                    result.SetStatus(eReturnStatusFailed);
                    return false;
                }
                else if (!UIntValueIsValidForSize (uval64, item_byte_size))
                {
                    result.AppendErrorWithFormat ("Value %" PRIo64 " is too large to fit in a %" PRIu64 " byte unsigned integer value.\n", uval64, (uint64_t)item_byte_size);
                    result.SetStatus(eReturnStatusFailed);
                    return false;
                }
                buffer.PutMaxHex64 (uval64, item_byte_size);
                break;
            }
        }

        if (!buffer.GetString().empty())
        {
            Error error;
            if (process->WriteMemory (addr, buffer.GetString().c_str(), buffer.GetString().size(), error) == buffer.GetString().size())
                return true;
            else
            {
                result.AppendErrorWithFormat ("Memory write to 0x%" PRIx64 " failed: %s.\n", addr, error.AsCString());
                result.SetStatus(eReturnStatusFailed);
                return false;
            }
        }
        return true;
    }

    OptionGroupOptions m_option_group;
    OptionGroupFormat m_format_options;
    OptionGroupWriteMemory m_memory_options;
};

//----------------------------------------------------------------------
// Get malloc/free history of a memory address.
//----------------------------------------------------------------------
class CommandObjectMemoryHistory : public CommandObjectParsed
{
public:
    CommandObjectMemoryHistory (CommandInterpreter &interpreter) :
        CommandObjectParsed(interpreter,
                            "memory history",
                            "Prints out the recorded stack traces for allocation/deallocation of a memory address.",
                            nullptr,
                            eCommandRequiresTarget | eCommandRequiresProcess | eCommandProcessMustBePaused |
                            eCommandProcessMustBeLaunched)
    {
        CommandArgumentEntry arg1;
        CommandArgumentData addr_arg;
        
        // Define the first (and only) variant of this arg.
        addr_arg.arg_type = eArgTypeAddress;
        addr_arg.arg_repetition = eArgRepeatPlain;
        
        // There is only one variant this argument could be; put it into the argument entry.
        arg1.push_back (addr_arg);
        
        // Push the data for the first argument into the m_arguments vector.
        m_arguments.push_back (arg1);
    }

    ~CommandObjectMemoryHistory() override = default;

    const char *
    GetRepeatCommand (Args &current_command_args, uint32_t index) override
    {
        return m_cmd_name.c_str();
    }
    
protected:
    bool
    DoExecute (Args& command, CommandReturnObject &result) override
    {
        const size_t argc = command.GetArgumentCount();
        
        if (argc == 0 || argc > 1)
        {
            result.AppendErrorWithFormat ("%s takes an address expression", m_cmd_name.c_str());
            result.SetStatus(eReturnStatusFailed);
            return false;
        }
        
        Error error;
        lldb::addr_t addr = Args::StringToAddress (&m_exe_ctx,
                                                   command.GetArgumentAtIndex(0),
                                                   LLDB_INVALID_ADDRESS,
                                                   &error);
        
        if (addr == LLDB_INVALID_ADDRESS)
        {
            result.AppendError("invalid address expression");
            result.AppendError(error.AsCString());
            result.SetStatus(eReturnStatusFailed);
            return false;
        }
        
        Stream *output_stream = &result.GetOutputStream();
        
        const ProcessSP &process_sp = m_exe_ctx.GetProcessSP();
        const MemoryHistorySP &memory_history = MemoryHistory::FindPlugin(process_sp);
        
        if (!memory_history)
        {
            result.AppendError("no available memory history provider");
            result.SetStatus(eReturnStatusFailed);
            return false;
        }
        
        HistoryThreads thread_list = memory_history->GetHistoryThreads(addr);
        
        for (auto thread : thread_list) {
            thread->GetStatus(*output_stream, 0, UINT32_MAX, 0);
        }
        
        result.SetStatus(eReturnStatusSuccessFinishResult);
        
        return true;
    }
};

//-------------------------------------------------------------------------
// CommandObjectMemory
//-------------------------------------------------------------------------

CommandObjectMemory::CommandObjectMemory (CommandInterpreter &interpreter) :
    CommandObjectMultiword (interpreter,
                            "memory",
                            "A set of commands for operating on memory.",
                            "memory <subcommand> [<subcommand-options>]")
{
    LoadSubCommand ("find", CommandObjectSP (new CommandObjectMemoryFind (interpreter)));
    LoadSubCommand ("read",  CommandObjectSP (new CommandObjectMemoryRead (interpreter)));
    LoadSubCommand ("write", CommandObjectSP (new CommandObjectMemoryWrite (interpreter)));
    LoadSubCommand ("history", CommandObjectSP (new CommandObjectMemoryHistory (interpreter)));
}

CommandObjectMemory::~CommandObjectMemory() = default;
