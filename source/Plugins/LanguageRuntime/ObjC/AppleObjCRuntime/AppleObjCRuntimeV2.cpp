//===-- AppleObjCRuntimeV2.cpp ----------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// C Includes
#include <stdint.h>

// C++ Includes
#include <string>
#include <vector>

// Other libraries and framework includes
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclObjC.h"

// Project includes
#include "lldb/lldb-enumerations.h"
#include "lldb/Core/ClangForward.h"
#include "lldb/Symbol/CompilerType.h"

#include "lldb/Core/ClangForward.h"
#include "lldb/Core/ConstString.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/Error.h"
#include "lldb/Core/Log.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Core/Scalar.h"
#include "lldb/Core/Section.h"
#include "lldb/Core/Stream.h"
#include "lldb/Core/StreamString.h"
#include "lldb/Core/Timer.h"
#include "lldb/Core/ValueObjectVariable.h"
#include "lldb/Expression/FunctionCaller.h"
#include "lldb/Expression/UtilityFunction.h"
#include "lldb/Host/StringConvert.h"
#include "lldb/Interpreter/CommandObject.h"
#include "lldb/Interpreter/CommandObjectMultiword.h"
#include "lldb/Interpreter/CommandReturnObject.h"
#include "lldb/Symbol/ClangASTContext.h"
#include "lldb/Symbol/ObjectFile.h"
#include "lldb/Symbol/Symbol.h"
#include "lldb/Symbol/TypeList.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/Platform.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/Thread.h"

#include "AppleObjCRuntimeV2.h"
#include "AppleObjCClassDescriptorV2.h"
#include "AppleObjCTypeEncodingParser.h"
#include "AppleObjCDeclVendor.h"
#include "AppleObjCTrampolineHandler.h"


using namespace lldb;
using namespace lldb_private;

// 2 second timeout when running utility functions
#define UTILITY_FUNCTION_TIMEOUT_USEC 2*1000*1000

static const char *g_get_dynamic_class_info_name = "__lldb_apple_objc_v2_get_dynamic_class_info";
// Testing using the new C++11 raw string literals. If this breaks GCC then we will
// need to revert to the code above...
static const char *g_get_dynamic_class_info_body = R"(

extern "C"
{
    size_t strlen(const char *);
    char *strncpy (char * s1, const char * s2, size_t n);
    int printf(const char * format, ...);
}
//#define ENABLE_DEBUG_PRINTF // COMMENT THIS LINE OUT PRIOR TO CHECKIN
#ifdef ENABLE_DEBUG_PRINTF
#define DEBUG_PRINTF(fmt, ...) printf(fmt, ## __VA_ARGS__)
#else
#define DEBUG_PRINTF(fmt, ...)
#endif

typedef struct _NXMapTable {
    void *prototype;
    unsigned num_classes;
    unsigned num_buckets_minus_one;
    void *buckets;
} NXMapTable;

#define NX_MAPNOTAKEY   ((void *)(-1))

typedef struct BucketInfo
{
    const char *name_ptr;
    Class isa;
} BucketInfo;

struct ClassInfo
{
    Class isa;
    uint32_t hash;
} __attribute__((__packed__));

uint32_t
__lldb_apple_objc_v2_get_dynamic_class_info (void *gdb_objc_realized_classes_ptr,
                                             void *class_infos_ptr,
                                             uint32_t class_infos_byte_size)
{
    DEBUG_PRINTF ("gdb_objc_realized_classes_ptr = %p\n", gdb_objc_realized_classes_ptr);
    DEBUG_PRINTF ("class_infos_ptr = %p\n", class_infos_ptr);
    DEBUG_PRINTF ("class_infos_byte_size = %u\n", class_infos_byte_size);
    const NXMapTable *grc = (const NXMapTable *)gdb_objc_realized_classes_ptr;
    if (grc)
    {
        const unsigned num_classes = grc->num_classes;
        if (class_infos_ptr)
        {
            const size_t max_class_infos = class_infos_byte_size/sizeof(ClassInfo);
            ClassInfo *class_infos = (ClassInfo *)class_infos_ptr;
            BucketInfo *buckets = (BucketInfo *)grc->buckets;
            
            uint32_t idx = 0;
            for (unsigned i=0; i<=grc->num_buckets_minus_one; ++i)
            {
                if (buckets[i].name_ptr != NX_MAPNOTAKEY)
                {
                    if (idx < max_class_infos)
                    {
                        const char *s = buckets[i].name_ptr;
                        uint32_t h = 5381;
                        for (unsigned char c = *s; c; c = *++s)
                            h = ((h << 5) + h) + c;
                        class_infos[idx].hash = h;
                        class_infos[idx].isa = buckets[i].isa;
                    }
                    ++idx;
                }
            }
            if (idx < max_class_infos)
            {
                class_infos[idx].isa = NULL;
                class_infos[idx].hash = 0;
            }
        }
        return num_classes;
    }
    return 0;
}

)";

static const char *g_get_shared_cache_class_info_name = "__lldb_apple_objc_v2_get_shared_cache_class_info";
// Testing using the new C++11 raw string literals. If this breaks GCC then we will
// need to revert to the code above...
static const char *g_get_shared_cache_class_info_body = R"(

extern "C"
{
    const char *class_getName(void *objc_class);
    size_t strlen(const char *);
    char *strncpy (char * s1, const char * s2, size_t n);
    int printf(const char * format, ...);
}

// #define ENABLE_DEBUG_PRINTF // COMMENT THIS LINE OUT PRIOR TO CHECKIN
#ifdef ENABLE_DEBUG_PRINTF
#define DEBUG_PRINTF(fmt, ...) printf(fmt, ## __VA_ARGS__)
#else
#define DEBUG_PRINTF(fmt, ...)
#endif


struct objc_classheader_t {
    int32_t clsOffset;
    int32_t hiOffset;
};

struct objc_clsopt_t {
    uint32_t capacity;
    uint32_t occupied;
    uint32_t shift;
    uint32_t mask;
    uint32_t zero;
    uint32_t unused;
    uint64_t salt;
    uint32_t scramble[256];
    uint8_t tab[0]; // tab[mask+1]
    //  uint8_t checkbytes[capacity];
    //  int32_t offset[capacity];
    //  objc_classheader_t clsOffsets[capacity];
    //  uint32_t duplicateCount;
    //  objc_classheader_t duplicateOffsets[duplicateCount];
};

struct objc_opt_t {
    uint32_t version;
    int32_t selopt_offset;
    int32_t headeropt_offset;
    int32_t clsopt_offset;
};

struct objc_opt_v14_t {
    uint32_t version;
    uint32_t flags;
    int32_t selopt_offset;
    int32_t headeropt_offset;
    int32_t clsopt_offset;
};

struct ClassInfo
{
    Class isa;
    uint32_t hash;
}  __attribute__((__packed__));

uint32_t
__lldb_apple_objc_v2_get_shared_cache_class_info (void *objc_opt_ro_ptr,
                                                  void *class_infos_ptr,
                                                  uint32_t class_infos_byte_size)
{
    uint32_t idx = 0;
    DEBUG_PRINTF ("objc_opt_ro_ptr = %p\n", objc_opt_ro_ptr);
    DEBUG_PRINTF ("class_infos_ptr = %p\n", class_infos_ptr);
    DEBUG_PRINTF ("class_infos_byte_size = %u (%" PRIu64 " class infos)\n", class_infos_byte_size, (size_t)(class_infos_byte_size/sizeof(ClassInfo)));
    if (objc_opt_ro_ptr)
    {
        const objc_opt_t *objc_opt = (objc_opt_t *)objc_opt_ro_ptr;
        const objc_opt_v14_t* objc_opt_v14 = (objc_opt_v14_t*)objc_opt_ro_ptr;
        const bool is_v14_format = objc_opt->version >= 14;
        if (is_v14_format)
        {
            DEBUG_PRINTF ("objc_opt->version = %u\n", objc_opt_v14->version);
            DEBUG_PRINTF ("objc_opt->flags = %u\n", objc_opt_v14->flags);
            DEBUG_PRINTF ("objc_opt->selopt_offset = %d\n", objc_opt_v14->selopt_offset);
            DEBUG_PRINTF ("objc_opt->headeropt_offset = %d\n", objc_opt_v14->headeropt_offset);
            DEBUG_PRINTF ("objc_opt->clsopt_offset = %d\n", objc_opt_v14->clsopt_offset);
        }
        else
        {
            DEBUG_PRINTF ("objc_opt->version = %u\n", objc_opt->version);
            DEBUG_PRINTF ("objc_opt->selopt_offset = %d\n", objc_opt->selopt_offset);
            DEBUG_PRINTF ("objc_opt->headeropt_offset = %d\n", objc_opt->headeropt_offset);
            DEBUG_PRINTF ("objc_opt->clsopt_offset = %d\n", objc_opt->clsopt_offset);
        }
        if (objc_opt->version == 12 || objc_opt->version == 13 || objc_opt->version == 14)
        {
            const objc_clsopt_t* clsopt = NULL;
            if (is_v14_format)
                clsopt = (const objc_clsopt_t*)((uint8_t *)objc_opt_v14 + objc_opt_v14->clsopt_offset);
            else
                clsopt = (const objc_clsopt_t*)((uint8_t *)objc_opt + objc_opt->clsopt_offset);
            const size_t max_class_infos = class_infos_byte_size/sizeof(ClassInfo);
            ClassInfo *class_infos = (ClassInfo *)class_infos_ptr;
            int32_t invalidEntryOffset = 0;
            // this is safe to do because the version field order is invariant
            if (objc_opt->version == 12)
                invalidEntryOffset = 16;
            const uint8_t *checkbytes = &clsopt->tab[clsopt->mask+1];
            const int32_t *offsets = (const int32_t *)(checkbytes + clsopt->capacity);
            const objc_classheader_t *classOffsets = (const objc_classheader_t *)(offsets + clsopt->capacity);
            DEBUG_PRINTF ("clsopt->capacity = %u\n", clsopt->capacity);
            DEBUG_PRINTF ("clsopt->mask = 0x%8.8x\n", clsopt->mask);
            DEBUG_PRINTF ("classOffsets = %p\n", classOffsets);
            for (uint32_t i=0; i<clsopt->capacity; ++i)
            {
                const int32_t clsOffset = classOffsets[i].clsOffset;
                if (clsOffset & 1)
                    continue; // duplicate
                else if (clsOffset == invalidEntryOffset)
                    continue; // invalid offset
                
                if (class_infos && idx < max_class_infos)
                {
                    class_infos[idx].isa = (Class)((uint8_t *)clsopt + clsOffset);
                    const char *name = class_getName (class_infos[idx].isa);
                    DEBUG_PRINTF ("[%u] isa = %8p %s\n", idx, class_infos[idx].isa, name);
                    // Hash the class name so we don't have to read it
                    const char *s = name;
                    uint32_t h = 5381;
                    for (unsigned char c = *s; c; c = *++s)
                        h = ((h << 5) + h) + c;
                    class_infos[idx].hash = h;
                }
                ++idx;
            }
            
            const uint32_t *duplicate_count_ptr = (uint32_t *)&classOffsets[clsopt->capacity];
            const uint32_t duplicate_count = *duplicate_count_ptr;
            const objc_classheader_t *duplicateClassOffsets = (const objc_classheader_t *)(&duplicate_count_ptr[1]);
            DEBUG_PRINTF ("duplicate_count = %u\n", duplicate_count);
            DEBUG_PRINTF ("duplicateClassOffsets = %p\n", duplicateClassOffsets);
            for (uint32_t i=0; i<duplicate_count; ++i)
            {
                const int32_t clsOffset = duplicateClassOffsets[i].clsOffset;
                if (clsOffset & 1)
                    continue; // duplicate
                else if (clsOffset == invalidEntryOffset)
                    continue; // invalid offset
                
                if (class_infos && idx < max_class_infos)
                {
                    class_infos[idx].isa = (Class)((uint8_t *)clsopt + clsOffset);
                    const char *name = class_getName (class_infos[idx].isa);
                    DEBUG_PRINTF ("[%u] isa = %8p %s\n", idx, class_infos[idx].isa, name);
                    // Hash the class name so we don't have to read it
                    const char *s = name;
                    uint32_t h = 5381;
                    for (unsigned char c = *s; c; c = *++s)
                        h = ((h << 5) + h) + c;
                    class_infos[idx].hash = h;
                }
                ++idx;
            }
        }
        DEBUG_PRINTF ("%u class_infos\n", idx);
        DEBUG_PRINTF ("done\n");
    }
    return idx;
}


)";

static uint64_t
ExtractRuntimeGlobalSymbol (Process* process,
                            ConstString name,
                            const ModuleSP &module_sp,
                            Error& error,
                            bool read_value = true,
                            uint8_t byte_size = 0,
                            uint64_t default_value = LLDB_INVALID_ADDRESS,
                            SymbolType sym_type = lldb::eSymbolTypeData)
{
    if (!process)
    {
        error.SetErrorString("no process");
        return default_value;
    }
    if (!module_sp)
    {
        error.SetErrorString("no module");
        return default_value;
    }
    if (!byte_size)
        byte_size = process->GetAddressByteSize();
    const Symbol *symbol = module_sp->FindFirstSymbolWithNameAndType(name, lldb::eSymbolTypeData);
    if (symbol && symbol->ValueIsAddress())
    {
        lldb::addr_t symbol_load_addr = symbol->GetAddressRef().GetLoadAddress(&process->GetTarget());
        if (symbol_load_addr != LLDB_INVALID_ADDRESS)
        {
            if (read_value)
                return process->ReadUnsignedIntegerFromMemory(symbol_load_addr, byte_size, default_value, error);
            else
                return symbol_load_addr;
        }
        else
        {
            error.SetErrorString("symbol address invalid");
            return default_value;
        }
    }
    else
    {
        error.SetErrorString("no symbol");
        return default_value;
    }
}

AppleObjCRuntimeV2::AppleObjCRuntimeV2 (Process *process,
                                        const ModuleSP &objc_module_sp) :
    AppleObjCRuntime (process),
    m_get_class_info_code(),
    m_get_class_info_args (LLDB_INVALID_ADDRESS),
    m_get_class_info_args_mutex (Mutex::eMutexTypeNormal),
    m_get_shared_cache_class_info_code(),
    m_get_shared_cache_class_info_args (LLDB_INVALID_ADDRESS),
    m_get_shared_cache_class_info_args_mutex (Mutex::eMutexTypeNormal),
    m_decl_vendor_ap (),
    m_isa_hash_table_ptr (LLDB_INVALID_ADDRESS),
    m_hash_signature (),
    m_has_object_getClass (false),
    m_loaded_objc_opt (false),
    m_non_pointer_isa_cache_ap(NonPointerISACache::CreateInstance(*this,objc_module_sp)),
    m_tagged_pointer_vendor_ap(TaggedPointerVendorV2::CreateInstance(*this,objc_module_sp)),
    m_encoding_to_type_sp(),
    m_noclasses_warning_emitted(false)
{
    static const ConstString g_gdb_object_getClass("gdb_object_getClass");
    m_has_object_getClass = (objc_module_sp->FindFirstSymbolWithNameAndType(g_gdb_object_getClass, eSymbolTypeCode) != NULL);
}

bool
AppleObjCRuntimeV2::GetDynamicTypeAndAddress (ValueObject &in_value,
                                              DynamicValueType use_dynamic, 
                                              TypeAndOrName &class_type_or_name, 
                                              Address &address,
                                              Value::ValueType &value_type)
{
    // We should never get here with a null process...
    assert (m_process != NULL);

    // The Runtime is attached to a particular process, you shouldn't pass in a value from another process.
    // Note, however, the process might be NULL (e.g. if the value was made with SBTarget::EvaluateExpression...)
    // in which case it is sufficient if the target's match:
    
    Process *process = in_value.GetProcessSP().get();
    if (process)
        assert (process == m_process);
    else
        assert (in_value.GetTargetSP().get() == m_process->CalculateTarget().get());
    
    class_type_or_name.Clear();
    value_type = Value::ValueType::eValueTypeScalar;

    // Make sure we can have a dynamic value before starting...
    if (CouldHaveDynamicValue (in_value))
    {
        // First job, pull out the address at 0 offset from the object  That will be the ISA pointer.
        ClassDescriptorSP objc_class_sp (GetNonKVOClassDescriptor (in_value));
        if (objc_class_sp)
        {
            const addr_t object_ptr = in_value.GetPointerValue();
            address.SetRawAddress(object_ptr);

            ConstString class_name (objc_class_sp->GetClassName());
            class_type_or_name.SetName(class_name);
            TypeSP type_sp (objc_class_sp->GetType());
            if (type_sp)
                class_type_or_name.SetTypeSP (type_sp);
            else
            {
                type_sp = LookupInCompleteClassCache (class_name);
                if (type_sp)
                {
                    objc_class_sp->SetType (type_sp);
                    class_type_or_name.SetTypeSP (type_sp);
                }
                else
                {
                    // try to go for a CompilerType at least
                    DeclVendor* vendor = GetDeclVendor();
                    if (vendor)
                    {
                        std::vector<clang::NamedDecl*> decls;
                        if (vendor->FindDecls(class_name, false, 1, decls) && decls.size())
                            class_type_or_name.SetCompilerType(ClangASTContext::GetTypeForDecl(decls[0]));
                    }
                }
            }
        }
    }    
    return class_type_or_name.IsEmpty() == false;
}

//------------------------------------------------------------------
// Static Functions
//------------------------------------------------------------------
LanguageRuntime *
AppleObjCRuntimeV2::CreateInstance (Process *process, LanguageType language)
{
    // FIXME: This should be a MacOS or iOS process, and we need to look for the OBJC section to make
    // sure we aren't using the V1 runtime.
    if (language == eLanguageTypeObjC)
    {
        ModuleSP objc_module_sp;
        
        if (AppleObjCRuntime::GetObjCVersion (process, objc_module_sp) == ObjCRuntimeVersions::eAppleObjC_V2)
            return new AppleObjCRuntimeV2 (process, objc_module_sp);
        else
            return NULL;
    }
    else
        return NULL;
}

class CommandObjectObjC_ClassTable_Dump : public CommandObjectParsed
{
public:
    CommandObjectObjC_ClassTable_Dump (CommandInterpreter &interpreter) :
    CommandObjectParsed (interpreter,
                         "dump",
                         "Dump information on Objective-C classes known to the current process.",
                         "language objc class-table dump",
                         eCommandRequiresProcess       |
                         eCommandProcessMustBeLaunched |
                         eCommandProcessMustBePaused   )
    {
    }

    ~CommandObjectObjC_ClassTable_Dump() override = default;

protected:
    bool
    DoExecute(Args& command, CommandReturnObject &result) override
    {
        Process *process = m_exe_ctx.GetProcessPtr();
        ObjCLanguageRuntime *objc_runtime = process->GetObjCLanguageRuntime();
        if (objc_runtime)
        {
            auto iterators_pair = objc_runtime->GetDescriptorIteratorPair();
            auto iterator = iterators_pair.first;
            for(; iterator != iterators_pair.second; iterator++)
            {
                result.GetOutputStream().Printf("isa = 0x%" PRIx64, iterator->first);
                if (iterator->second)
                {
                    result.GetOutputStream().Printf(" name = %s", iterator->second->GetClassName().AsCString("<unknown>"));
                    result.GetOutputStream().Printf(" instance size = %" PRIu64, iterator->second->GetInstanceSize());
                    result.GetOutputStream().Printf(" num ivars = %" PRIuPTR, (uintptr_t)iterator->second->GetNumIVars());
                    if (auto superclass = iterator->second->GetSuperclass())
                    {
                        result.GetOutputStream().Printf(" superclass = %s", superclass->GetClassName().AsCString("<unknown>"));
                    }
                    result.GetOutputStream().Printf("\n");
                }
                else
                {
                    result.GetOutputStream().Printf(" has no associated class.\n");
                }
            }
            result.SetStatus(lldb::eReturnStatusSuccessFinishResult);
            return true;
        }
        else
        {
            result.AppendError("current process has no Objective-C runtime loaded");
            result.SetStatus(lldb::eReturnStatusFailed);
            return false;
        }
    }
};

class CommandObjectMultiwordObjC_TaggedPointer_Info : public CommandObjectParsed
{
public:
    CommandObjectMultiwordObjC_TaggedPointer_Info (CommandInterpreter &interpreter) :
    CommandObjectParsed (interpreter,
                         "info",
                         "Dump information on a tagged pointer.",
                         "language objc tagged-pointer info",
                         eCommandRequiresProcess       |
                         eCommandProcessMustBeLaunched |
                         eCommandProcessMustBePaused   )
    {
        CommandArgumentEntry arg;
        CommandArgumentData index_arg;
        
        // Define the first (and only) variant of this arg.
        index_arg.arg_type = eArgTypeAddress;
        index_arg.arg_repetition = eArgRepeatPlus;
        
        // There is only one variant this argument could be; put it into the argument entry.
        arg.push_back (index_arg);
        
        // Push the data for the first argument into the m_arguments vector.
        m_arguments.push_back (arg);
    }

    ~CommandObjectMultiwordObjC_TaggedPointer_Info() override = default;

protected:
    bool
    DoExecute(Args& command, CommandReturnObject &result) override
    {
        if (command.GetArgumentCount() == 0)
        {
            result.AppendError("this command requires arguments");
            result.SetStatus(lldb::eReturnStatusFailed);
            return false;
        }
        
        Process *process = m_exe_ctx.GetProcessPtr();
        ExecutionContext exe_ctx(process);
        ObjCLanguageRuntime *objc_runtime = process->GetObjCLanguageRuntime();
        if (objc_runtime)
        {
            ObjCLanguageRuntime::TaggedPointerVendor *tagged_ptr_vendor = objc_runtime->GetTaggedPointerVendor();
            if (tagged_ptr_vendor)
            {
                for (size_t i = 0;
                     i < command.GetArgumentCount();
                     i++)
                {
                    const char *arg_str = command.GetArgumentAtIndex(i);
                    if (!arg_str)
                        continue;
                    Error error;
                    lldb::addr_t arg_addr = Args::StringToAddress(&exe_ctx, arg_str, LLDB_INVALID_ADDRESS, &error);
                    if (arg_addr == 0 || arg_addr == LLDB_INVALID_ADDRESS || error.Fail())
                        continue;
                    auto descriptor_sp = tagged_ptr_vendor->GetClassDescriptor(arg_addr);
                    if (!descriptor_sp)
                        continue;
                    uint64_t info_bits = 0;
                    uint64_t value_bits = 0;
                    uint64_t payload = 0;
                    if (descriptor_sp->GetTaggedPointerInfo(&info_bits, &value_bits, &payload))
                    {
                        result.GetOutputStream().Printf("0x%" PRIx64 " is tagged.\n\tpayload = 0x%" PRIx64 "\n\tvalue = 0x%" PRIx64 "\n\tinfo bits = 0x%" PRIx64 "\n\tclass = %s\n",
                                                        (uint64_t)arg_addr,
                                                        payload,
                                                        value_bits,
                                                        info_bits,
                                                        descriptor_sp->GetClassName().AsCString("<unknown>"));
                    }
                    else
                    {
                        result.GetOutputStream().Printf("0x%" PRIx64 " is not tagged.\n", (uint64_t)arg_addr);
                    }
                }
            }
            else
            {
                result.AppendError("current process has no tagged pointer support");
                result.SetStatus(lldb::eReturnStatusFailed);
                return false;
            }
            result.SetStatus(lldb::eReturnStatusSuccessFinishResult);
            return true;
        }
        else
        {
            result.AppendError("current process has no Objective-C runtime loaded");
            result.SetStatus(lldb::eReturnStatusFailed);
            return false;
        }
    }
};

class CommandObjectMultiwordObjC_ClassTable : public CommandObjectMultiword
{
public:
    CommandObjectMultiwordObjC_ClassTable (CommandInterpreter &interpreter) :
    CommandObjectMultiword (interpreter,
                            "class-table",
                            "A set of commands for operating on the Objective-C class table.",
                            "class-table <subcommand> [<subcommand-options>]")
    {
        LoadSubCommand ("dump",   CommandObjectSP (new CommandObjectObjC_ClassTable_Dump (interpreter)));
    }

    ~CommandObjectMultiwordObjC_ClassTable() override = default;
};

class CommandObjectMultiwordObjC_TaggedPointer : public CommandObjectMultiword
{
public:
    
    CommandObjectMultiwordObjC_TaggedPointer (CommandInterpreter &interpreter) :
    CommandObjectMultiword (interpreter,
                            "tagged-pointer",
                            "A set of commands for operating on Objective-C tagged pointers.",
                            "class-table <subcommand> [<subcommand-options>]")
    {
        LoadSubCommand ("info",   CommandObjectSP (new CommandObjectMultiwordObjC_TaggedPointer_Info (interpreter)));
    }

    ~CommandObjectMultiwordObjC_TaggedPointer() override = default;
};

class CommandObjectMultiwordObjC : public CommandObjectMultiword
{
public:
    CommandObjectMultiwordObjC (CommandInterpreter &interpreter) :
    CommandObjectMultiword (interpreter,
                            "objc",
                            "A set of commands for operating on the Objective-C Language Runtime.",
                            "objc <subcommand> [<subcommand-options>]")
    {
        LoadSubCommand ("class-table",   CommandObjectSP (new CommandObjectMultiwordObjC_ClassTable (interpreter)));
        LoadSubCommand ("tagged-pointer",   CommandObjectSP (new CommandObjectMultiwordObjC_TaggedPointer (interpreter)));
    }

    ~CommandObjectMultiwordObjC() override = default;
};

void
AppleObjCRuntimeV2::Initialize()
{
    PluginManager::RegisterPlugin (GetPluginNameStatic(),
                                   "Apple Objective C Language Runtime - Version 2",
                                   CreateInstance,
                                   [] (CommandInterpreter& interpreter) -> lldb::CommandObjectSP {
                                       return CommandObjectSP(new CommandObjectMultiwordObjC(interpreter));
                                   });
}

void
AppleObjCRuntimeV2::Terminate()
{
    PluginManager::UnregisterPlugin (CreateInstance);
}

lldb_private::ConstString
AppleObjCRuntimeV2::GetPluginNameStatic()
{
    static ConstString g_name("apple-objc-v2");
    return g_name;
}

//------------------------------------------------------------------
// PluginInterface protocol
//------------------------------------------------------------------
lldb_private::ConstString
AppleObjCRuntimeV2::GetPluginName()
{
    return GetPluginNameStatic();
}

uint32_t
AppleObjCRuntimeV2::GetPluginVersion()
{
    return 1;
}

BreakpointResolverSP
AppleObjCRuntimeV2::CreateExceptionResolver (Breakpoint *bkpt, bool catch_bp, bool throw_bp)
{
    BreakpointResolverSP resolver_sp;
    
    if (throw_bp)
        resolver_sp.reset (new BreakpointResolverName (bkpt,
                                                       "objc_exception_throw",
                                                       eFunctionNameTypeBase,
                                                       eLanguageTypeUnknown,
                                                       Breakpoint::Exact,
                                                       eLazyBoolNo));
    // FIXME: We don't do catch breakpoints for ObjC yet.
    // Should there be some way for the runtime to specify what it can do in this regard?
    return resolver_sp;
}

UtilityFunction *
AppleObjCRuntimeV2::CreateObjectChecker(const char *name)
{
    char check_function_code[2048];
    
    int len = 0;
    if (m_has_object_getClass)
    {
        len = ::snprintf (check_function_code, 
                          sizeof(check_function_code),
                          "extern \"C\" void *gdb_object_getClass(void *);                                          \n"
                          "extern \"C\"  int printf(const char *format, ...);                                       \n"
                          "extern \"C\" void                                                                        \n"
                          "%s(void *$__lldb_arg_obj, void *$__lldb_arg_selector)                                    \n"
                          "{                                                                                        \n"
                          "   if ($__lldb_arg_obj == (void *)0)                                                     \n"
                          "       return; // nil is ok                                                              \n" 
                          "   if (!gdb_object_getClass($__lldb_arg_obj))                                            \n"
                          "       *((volatile int *)0) = 'ocgc';                                                    \n"
                          "   else if ($__lldb_arg_selector != (void *)0)                                           \n"
                          "   {                                                                                     \n"
                          "        signed char responds = (signed char) [(id) $__lldb_arg_obj                       \n"
                          "                                                respondsToSelector:                      \n"
                          "                                       (struct objc_selector *) $__lldb_arg_selector];   \n"
                          "       if (responds == (signed char) 0)                                                  \n"
                          "           *((volatile int *)0) = 'ocgc';                                                \n"
                          "   }                                                                                     \n"
                          "}                                                                                        \n",
                          name);
    }
    else
    {
        len = ::snprintf (check_function_code, 
                          sizeof(check_function_code), 
                          "extern \"C\" void *gdb_class_getClass(void *);                                           \n"
                          "extern \"C\"  int printf(const char *format, ...);                                       \n"
                          "extern \"C\"  void                                                                       \n"
                          "%s(void *$__lldb_arg_obj, void *$__lldb_arg_selector)                                    \n"
                          "{                                                                                        \n"
                          "   if ($__lldb_arg_obj == (void *)0)                                                     \n"
                          "       return; // nil is ok                                                              \n" 
                          "    void **$isa_ptr = (void **)$__lldb_arg_obj;                                          \n"
                          "    if (*$isa_ptr == (void *)0 || !gdb_class_getClass(*$isa_ptr))                        \n"
                          "       *((volatile int *)0) = 'ocgc';                                                    \n"
                          "   else if ($__lldb_arg_selector != (void *)0)                                           \n"
                          "   {                                                                                     \n"
                          "        signed char responds = (signed char) [(id) $__lldb_arg_obj                       \n"
                          "                                                respondsToSelector:                      \n"
                          "                                        (struct objc_selector *) $__lldb_arg_selector];  \n"
                          "       if (responds == (signed char) 0)                                                  \n"
                          "           *((volatile int *)0) = 'ocgc';                                                \n"
                          "   }                                                                                     \n"
                          "}                                                                                        \n", 
                          name);
    }
    
    assert (len < (int)sizeof(check_function_code));

    Error error;
    return GetTargetRef().GetUtilityFunctionForLanguage(check_function_code, eLanguageTypeObjC, name, error);
}

size_t
AppleObjCRuntimeV2::GetByteOffsetForIvar (CompilerType &parent_ast_type, const char *ivar_name)
{
    uint32_t ivar_offset = LLDB_INVALID_IVAR_OFFSET;

    const char *class_name = parent_ast_type.GetConstTypeName().AsCString();
    if (class_name && class_name[0] && ivar_name && ivar_name[0])
    {
        //----------------------------------------------------------------------
        // Make the objective C V2 mangled name for the ivar offset from the
        // class name and ivar name
        //----------------------------------------------------------------------
        std::string buffer("OBJC_IVAR_$_");
        buffer.append (class_name);
        buffer.push_back ('.');
        buffer.append (ivar_name);
        ConstString ivar_const_str (buffer.c_str());
        
        //----------------------------------------------------------------------
        // Try to get the ivar offset address from the symbol table first using
        // the name we created above
        //----------------------------------------------------------------------
        SymbolContextList sc_list;
        Target &target = m_process->GetTarget();
        target.GetImages().FindSymbolsWithNameAndType(ivar_const_str, eSymbolTypeObjCIVar, sc_list);

        addr_t ivar_offset_address = LLDB_INVALID_ADDRESS;

        Error error;
        SymbolContext ivar_offset_symbol;
        if (sc_list.GetSize() == 1 && sc_list.GetContextAtIndex(0, ivar_offset_symbol))
        {
            if (ivar_offset_symbol.symbol)
                ivar_offset_address = ivar_offset_symbol.symbol->GetLoadAddress (&target);
        }

        //----------------------------------------------------------------------
        // If we didn't get the ivar offset address from the symbol table, fall
        // back to getting it from the runtime
        //----------------------------------------------------------------------
        if (ivar_offset_address == LLDB_INVALID_ADDRESS)
            ivar_offset_address = LookupRuntimeSymbol(ivar_const_str);

        if (ivar_offset_address != LLDB_INVALID_ADDRESS)
            ivar_offset = m_process->ReadUnsignedIntegerFromMemory (ivar_offset_address,
                                                                    4,
                                                                    LLDB_INVALID_IVAR_OFFSET,
                                                                    error);
    }
    return ivar_offset;
}

// tagged pointers are special not-a-real-pointer values that contain both type and value information
// this routine attempts to check with as little computational effort as possible whether something
// could possibly be a tagged pointer - false positives are possible but false negatives shouldn't
bool
AppleObjCRuntimeV2::IsTaggedPointer(addr_t ptr)
{
    if (!m_tagged_pointer_vendor_ap)
        return false;
    return m_tagged_pointer_vendor_ap->IsPossibleTaggedPointer(ptr);
}

class RemoteNXMapTable
{
public:
    RemoteNXMapTable () :
        m_count (0),
        m_num_buckets_minus_one (0),
        m_buckets_ptr (LLDB_INVALID_ADDRESS),
        m_process (NULL),
        m_end_iterator (*this, -1),
        m_load_addr (LLDB_INVALID_ADDRESS),
        m_map_pair_size (0),
        m_invalid_key (0)
    {
    }
    
    void
    Dump ()
    {
        printf ("RemoteNXMapTable.m_load_addr = 0x%" PRIx64 "\n", m_load_addr);
        printf ("RemoteNXMapTable.m_count = %u\n", m_count);
        printf ("RemoteNXMapTable.m_num_buckets_minus_one = %u\n", m_num_buckets_minus_one);
        printf ("RemoteNXMapTable.m_buckets_ptr = 0x%" PRIX64 "\n", m_buckets_ptr);
    }
    
    bool
    ParseHeader (Process* process, lldb::addr_t load_addr)
    {
        m_process = process;
        m_load_addr = load_addr;
        m_map_pair_size = m_process->GetAddressByteSize() * 2;
        m_invalid_key = m_process->GetAddressByteSize() == 8 ? UINT64_MAX : UINT32_MAX;
        Error err;
        
        // This currently holds true for all platforms we support, but we might
        // need to change this to use get the actually byte size of "unsigned"
        // from the target AST...
        const uint32_t unsigned_byte_size = sizeof(uint32_t);
        // Skip the prototype as we don't need it (const struct +NXMapTablePrototype *prototype)
        
        bool success = true;
        if (load_addr == LLDB_INVALID_ADDRESS)
            success = false;
        else
        {
            lldb::addr_t cursor = load_addr + m_process->GetAddressByteSize();
                    
            // unsigned count;
            m_count = m_process->ReadUnsignedIntegerFromMemory(cursor, unsigned_byte_size, 0, err);
            if (m_count)
            {
                cursor += unsigned_byte_size;
            
                // unsigned nbBucketsMinusOne;
                m_num_buckets_minus_one = m_process->ReadUnsignedIntegerFromMemory(cursor, unsigned_byte_size, 0, err);
                cursor += unsigned_byte_size;
            
                // void *buckets;
                m_buckets_ptr = m_process->ReadPointerFromMemory(cursor, err);
                
                success = m_count > 0 && m_buckets_ptr != LLDB_INVALID_ADDRESS;
            }
        }
        
        if (!success)
        {
            m_count = 0;
            m_num_buckets_minus_one = 0;
            m_buckets_ptr = LLDB_INVALID_ADDRESS;
        }
        return success;
    }
    
    // const_iterator mimics NXMapState and its code comes from NXInitMapState and NXNextMapState.
    typedef std::pair<ConstString, ObjCLanguageRuntime::ObjCISA> element;

    friend class const_iterator;
    class const_iterator
    {
    public:
        const_iterator (RemoteNXMapTable &parent, int index) : m_parent(parent), m_index(index)
        {
            AdvanceToValidIndex();
        }
        
        const_iterator (const const_iterator &rhs) : m_parent(rhs.m_parent), m_index(rhs.m_index)
        {
            // AdvanceToValidIndex() has been called by rhs already.
        }
        
        const_iterator &operator=(const const_iterator &rhs)
        {
            // AdvanceToValidIndex() has been called by rhs already.
            assert (&m_parent == &rhs.m_parent);
            m_index = rhs.m_index;
            return *this;
        }
        
        bool operator==(const const_iterator &rhs) const
        {
            if (&m_parent != &rhs.m_parent)
                return false;
            if (m_index != rhs.m_index)
                return false;
            
            return true;
        }
        
        bool operator!=(const const_iterator &rhs) const
        {
            return !(operator==(rhs));
        }
        
        const_iterator &operator++()
        {
            AdvanceToValidIndex();
            return *this;
        }
        
        const element operator*() const
        {
            if (m_index == -1)
            {
                // TODO find a way to make this an error, but not an assert
                return element();
            }
         
            lldb::addr_t pairs_ptr = m_parent.m_buckets_ptr;
            size_t map_pair_size = m_parent.m_map_pair_size;
            lldb::addr_t pair_ptr = pairs_ptr + (m_index * map_pair_size);
            
            Error err;
            
            lldb::addr_t key = m_parent.m_process->ReadPointerFromMemory(pair_ptr, err);
            if (!err.Success())
                return element();
            lldb::addr_t value = m_parent.m_process->ReadPointerFromMemory(pair_ptr + m_parent.m_process->GetAddressByteSize(), err);
            if (!err.Success())
                return element();
            
            std::string key_string;
            
            m_parent.m_process->ReadCStringFromMemory(key, key_string, err);
            if (!err.Success())
                return element();
            
            return element(ConstString(key_string.c_str()), (ObjCLanguageRuntime::ObjCISA)value);
        }

    private:
        void AdvanceToValidIndex ()
        {
            if (m_index == -1)
                return;
            
            const lldb::addr_t pairs_ptr = m_parent.m_buckets_ptr;
            const size_t map_pair_size = m_parent.m_map_pair_size;
            const lldb::addr_t invalid_key = m_parent.m_invalid_key;
            Error err;

            while (m_index--)
            {
                lldb::addr_t pair_ptr = pairs_ptr + (m_index * map_pair_size);
                lldb::addr_t key = m_parent.m_process->ReadPointerFromMemory(pair_ptr, err);
                
                if (!err.Success())
                {
                    m_index = -1;
                    return;
                }
                
                if (key != invalid_key)
                    return;
            }
        }
        RemoteNXMapTable   &m_parent;
        int                 m_index;
    };
    
    const_iterator begin ()
    {
        return const_iterator(*this, m_num_buckets_minus_one + 1);
    }
    
    const_iterator end ()
    {
        return m_end_iterator;
    }
    
    uint32_t
    GetCount () const
    {
        return m_count;
    }
    
    uint32_t
    GetBucketCount () const
    {
        return m_num_buckets_minus_one;
    }
    
    lldb::addr_t
    GetBucketDataPointer () const
    {
        return m_buckets_ptr;
    }
    
    lldb::addr_t
    GetTableLoadAddress() const
    {
        return m_load_addr;
    }

private:
    // contents of _NXMapTable struct
    uint32_t m_count;
    uint32_t m_num_buckets_minus_one;
    lldb::addr_t m_buckets_ptr;
    lldb_private::Process *m_process;
    const_iterator m_end_iterator;
    lldb::addr_t m_load_addr;
    size_t m_map_pair_size;
    lldb::addr_t m_invalid_key;
};

AppleObjCRuntimeV2::HashTableSignature::HashTableSignature() :
    m_count (0),
    m_num_buckets (0),
    m_buckets_ptr (0)
{
}

void
AppleObjCRuntimeV2::HashTableSignature::UpdateSignature (const RemoteNXMapTable &hash_table)
{
    m_count = hash_table.GetCount();
    m_num_buckets = hash_table.GetBucketCount();
    m_buckets_ptr = hash_table.GetBucketDataPointer();
}

bool
AppleObjCRuntimeV2::HashTableSignature::NeedsUpdate (Process *process, AppleObjCRuntimeV2 *runtime, RemoteNXMapTable &hash_table)
{
    if (!hash_table.ParseHeader(process, runtime->GetISAHashTablePointer ()))
    {
        return false; // Failed to parse the header, no need to update anything
    }

    // Check with out current signature and return true if the count,
    // number of buckets or the hash table address changes.
    if (m_count == hash_table.GetCount() &&
        m_num_buckets == hash_table.GetBucketCount() &&
        m_buckets_ptr == hash_table.GetBucketDataPointer())
    {
        // Hash table hasn't changed
        return false;
    }
    // Hash table data has changed, we need to update
    return true;
}

ObjCLanguageRuntime::ClassDescriptorSP
AppleObjCRuntimeV2::GetClassDescriptorFromISA (ObjCISA isa)
{
    ObjCLanguageRuntime::ClassDescriptorSP class_descriptor_sp;
    if (m_non_pointer_isa_cache_ap.get())
        class_descriptor_sp = m_non_pointer_isa_cache_ap->GetClassDescriptor(isa);
    if (!class_descriptor_sp)
        class_descriptor_sp = ObjCLanguageRuntime::GetClassDescriptorFromISA(isa);
    return class_descriptor_sp;
}

ObjCLanguageRuntime::ClassDescriptorSP
AppleObjCRuntimeV2::GetClassDescriptor (ValueObject& valobj)
{
    ClassDescriptorSP objc_class_sp;
    if (valobj.IsBaseClass())
    {
        ValueObject *parent = valobj.GetParent();
        // if I am my own parent, bail out of here fast..
        if (parent && parent != &valobj)
        {
            ClassDescriptorSP parent_descriptor_sp = GetClassDescriptor(*parent);
            if (parent_descriptor_sp)
                return parent_descriptor_sp->GetSuperclass();
        }
        return nullptr;
    }
    // if we get an invalid VO (which might still happen when playing around
    // with pointers returned by the expression parser, don't consider this
    // a valid ObjC object)
    if (valobj.GetCompilerType().IsValid())
    {
        addr_t isa_pointer = valobj.GetPointerValue();
        
        // tagged pointer
        if (IsTaggedPointer(isa_pointer))
        {
            return m_tagged_pointer_vendor_ap->GetClassDescriptor(isa_pointer);
        }
        else
        {
            ExecutionContext exe_ctx (valobj.GetExecutionContextRef());
            
            Process *process = exe_ctx.GetProcessPtr();
            if (process)
            {
                Error error;
                ObjCISA isa = process->ReadPointerFromMemory(isa_pointer, error);
                if (isa != LLDB_INVALID_ADDRESS)
                {
                    objc_class_sp = GetClassDescriptorFromISA (isa);
                    if (isa && !objc_class_sp)
                    {
                        Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_PROCESS));
                        if (log)
                            log->Printf("0x%" PRIx64 ": AppleObjCRuntimeV2::GetClassDescriptor() ISA was not in class descriptor cache 0x%" PRIx64,
                                        isa_pointer,
                                        isa);
                    }
                }
            }
        }
    }
    return objc_class_sp;
}

lldb::addr_t
AppleObjCRuntimeV2::GetISAHashTablePointer ()
{
    if (m_isa_hash_table_ptr == LLDB_INVALID_ADDRESS)
    {
        Process *process = GetProcess();

        ModuleSP objc_module_sp(GetObjCModule());
        
        if (!objc_module_sp)
            return LLDB_INVALID_ADDRESS;

        static ConstString g_gdb_objc_realized_classes("gdb_objc_realized_classes");
        
        const Symbol *symbol = objc_module_sp->FindFirstSymbolWithNameAndType(g_gdb_objc_realized_classes, lldb::eSymbolTypeAny);
        if (symbol)
        {
            lldb::addr_t gdb_objc_realized_classes_ptr = symbol->GetLoadAddress(&process->GetTarget());
            
            if (gdb_objc_realized_classes_ptr != LLDB_INVALID_ADDRESS)
            {
                Error error;
                m_isa_hash_table_ptr = process->ReadPointerFromMemory(gdb_objc_realized_classes_ptr, error);
            }
        }
    }
    return m_isa_hash_table_ptr;
}

bool
AppleObjCRuntimeV2::UpdateISAToDescriptorMapDynamic(RemoteNXMapTable &hash_table)
{
    Process *process = GetProcess();
    
    if (process == NULL)
        return false;
    
    Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_PROCESS));
    
    ExecutionContext exe_ctx;
    
    ThreadSP thread_sp = process->GetThreadList().GetSelectedThread();
    
    if (!thread_sp)
        return false;
    
    thread_sp->CalculateExecutionContext(exe_ctx);
    ClangASTContext *ast = process->GetTarget().GetScratchClangASTContext();
    
    if (!ast)
        return false;
    
    Address function_address;
    
    StreamString errors;
    
    const uint32_t addr_size = process->GetAddressByteSize();
    
    Error err;
    
    // Read the total number of classes from the hash table
    const uint32_t num_classes = hash_table.GetCount();
    if (num_classes == 0)
    {
        if (log)
            log->Printf ("No dynamic classes found in gdb_objc_realized_classes.");
        return false;
    }
    
    // Make some types for our arguments
    CompilerType clang_uint32_t_type = ast->GetBuiltinTypeForEncodingAndBitSize(eEncodingUint, 32);
    CompilerType clang_void_pointer_type = ast->GetBasicType(eBasicTypeVoid).GetPointerType();
    
    ValueList arguments;
    FunctionCaller *get_class_info_function = nullptr;

    if (!m_get_class_info_code.get())
    {
        Error error;
        m_get_class_info_code.reset (GetTargetRef().GetUtilityFunctionForLanguage (g_get_dynamic_class_info_body,
                                                                                   eLanguageTypeObjC,
                                                                                   g_get_dynamic_class_info_name,
                                                                                   error));
        if (error.Fail())
        {
            if (log)
                log->Printf ("Failed to get Utility Function for implementation lookup: %s", error.AsCString());
            m_get_class_info_code.reset();
        }
        else
        {
            errors.Clear();
            
            if (!m_get_class_info_code->Install(errors, exe_ctx))
            {
                if (log)
                    log->Printf ("Failed to install implementation lookup: %s.", errors.GetData());
                m_get_class_info_code.reset();
            }
        }
        if (!m_get_class_info_code.get())
            return false;
        
        // Next make the runner function for our implementation utility function.
        Value value;
        value.SetValueType (Value::eValueTypeScalar);
        value.SetCompilerType (clang_void_pointer_type);
        arguments.PushValue (value);
        arguments.PushValue (value);
        
        value.SetValueType (Value::eValueTypeScalar);
        value.SetCompilerType (clang_uint32_t_type);
        arguments.PushValue (value);
        
        get_class_info_function = m_get_class_info_code->MakeFunctionCaller(clang_uint32_t_type,
                                                                            arguments,
                                                                            error);
        
        if (error.Fail())
        {
            if (log)
                log->Printf("Failed to make function caller for implementation lookup: %s.", error.AsCString());
            return false;
        }
    }
    else
    {
        get_class_info_function = m_get_class_info_code->GetFunctionCaller();
        if (!get_class_info_function)
        {
            if (log)
                log->Printf ("Failed to get implementation lookup function caller: %s.", errors.GetData());
            return false;
        }
        arguments = get_class_info_function->GetArgumentValues();
    }

    errors.Clear();
    
    const uint32_t class_info_byte_size = addr_size + 4;
    const uint32_t class_infos_byte_size = num_classes * class_info_byte_size;
    lldb::addr_t class_infos_addr = process->AllocateMemory(class_infos_byte_size,
                                                            ePermissionsReadable | ePermissionsWritable,
                                                            err);
    
    if (class_infos_addr == LLDB_INVALID_ADDRESS)
        return false;
    
    Mutex::Locker locker(m_get_class_info_args_mutex);
    
    // Fill in our function argument values
    arguments.GetValueAtIndex(0)->GetScalar() = hash_table.GetTableLoadAddress();
    arguments.GetValueAtIndex(1)->GetScalar() = class_infos_addr;
    arguments.GetValueAtIndex(2)->GetScalar() = class_infos_byte_size;
    
    bool success = false;
    
    errors.Clear();
    
    // Write our function arguments into the process so we can run our function
    if (get_class_info_function->WriteFunctionArguments (exe_ctx,
                                                           m_get_class_info_args,
                                                           arguments,
                                                           errors))
    {
        EvaluateExpressionOptions options;
        options.SetUnwindOnError(true);
        options.SetTryAllThreads(false);
        options.SetStopOthers(true);
        options.SetIgnoreBreakpoints(true);
        options.SetTimeoutUsec(UTILITY_FUNCTION_TIMEOUT_USEC);
        
        Value return_value;
        return_value.SetValueType (Value::eValueTypeScalar);
        //return_value.SetContext (Value::eContextTypeClangType, clang_uint32_t_type);
        return_value.SetCompilerType (clang_uint32_t_type);
        return_value.GetScalar() = 0;
        
        errors.Clear();
        
        // Run the function
        ExpressionResults results = get_class_info_function->ExecuteFunction (exe_ctx,
                                                                              &m_get_class_info_args,
                                                                              options,
                                                                              errors,
                                                                              return_value);
        
        if (results == eExpressionCompleted)
        {
            // The result is the number of ClassInfo structures that were filled in
            uint32_t num_class_infos = return_value.GetScalar().ULong();
            if (log)
                log->Printf("Discovered %u ObjC classes\n",num_class_infos);
            if (num_class_infos > 0)
            {
                // Read the ClassInfo structures
                DataBufferHeap buffer (num_class_infos * class_info_byte_size, 0);
                if (process->ReadMemory(class_infos_addr, buffer.GetBytes(), buffer.GetByteSize(), err) == buffer.GetByteSize())
                {
                    DataExtractor class_infos_data (buffer.GetBytes(),
                                                    buffer.GetByteSize(),
                                                    process->GetByteOrder(),
                                                    addr_size);
                    ParseClassInfoArray (class_infos_data, num_class_infos);
                }
            }
            success = true;
        }
        else
        {
            if (log)
                log->Printf("Error evaluating our find class name function: %s.\n", errors.GetData());
        }
    }
    else
    {
        if (log)
            log->Printf ("Error writing function arguments: \"%s\".", errors.GetData());
    }
    
    // Deallocate the memory we allocated for the ClassInfo array
    process->DeallocateMemory(class_infos_addr);
    
    return success;
}

uint32_t
AppleObjCRuntimeV2::ParseClassInfoArray (const DataExtractor &data, uint32_t num_class_infos)
{
    // Parses an array of "num_class_infos" packed ClassInfo structures:
    //
    //    struct ClassInfo
    //    {
    //        Class isa;
    //        uint32_t hash;
    //    } __attribute__((__packed__));

    Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_PROCESS));
    
    uint32_t num_parsed = 0;

    // Iterate through all ClassInfo structures
    lldb::offset_t offset = 0;
    for (uint32_t i=0; i<num_class_infos; ++i)
    {
        ObjCISA isa = data.GetPointer(&offset);
        
        if (isa == 0)
        {
            if (log)
                log->Printf("AppleObjCRuntimeV2 found NULL isa, ignoring this class info");
            continue;
        }
        // Check if we already know about this ISA, if we do, the info will
        // never change, so we can just skip it.
        if (ISAIsCached(isa))
        {
            offset += 4;
        }
        else
        {
            // Read the 32 bit hash for the class name
            const uint32_t name_hash = data.GetU32(&offset);
            ClassDescriptorSP descriptor_sp (new ClassDescriptorV2(*this, isa, NULL));
            AddClass (isa, descriptor_sp, name_hash);
            num_parsed++;
            if (log && log->GetVerbose())
                log->Printf("AppleObjCRuntimeV2 added isa=0x%" PRIx64 ", hash=0x%8.8x, name=%s", isa, name_hash,descriptor_sp->GetClassName().AsCString("<unknown>"));
        }
    }
    return num_parsed;
}

AppleObjCRuntimeV2::DescriptorMapUpdateResult
AppleObjCRuntimeV2::UpdateISAToDescriptorMapSharedCache()
{
    Process *process = GetProcess();
    
    if (process == NULL)
        return DescriptorMapUpdateResult::Fail();
    
    Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_PROCESS));
    
    ExecutionContext exe_ctx;
    
    ThreadSP thread_sp = process->GetThreadList().GetSelectedThread();
    
    if (!thread_sp)
        return DescriptorMapUpdateResult::Fail();
    
    thread_sp->CalculateExecutionContext(exe_ctx);
    ClangASTContext *ast = process->GetTarget().GetScratchClangASTContext();
    
    if (!ast)
        return DescriptorMapUpdateResult::Fail();
    
    Address function_address;
    
    StreamString errors;
    
    const uint32_t addr_size = process->GetAddressByteSize();
    
    Error err;
    
    const lldb::addr_t objc_opt_ptr = GetSharedCacheReadOnlyAddress();
    
    if (objc_opt_ptr == LLDB_INVALID_ADDRESS)
        return DescriptorMapUpdateResult::Fail();
    
    // Read the total number of classes from the hash table
    const uint32_t num_classes = 128*1024;
    if (num_classes == 0)
    {
        if (log)
            log->Printf ("No dynamic classes found in gdb_objc_realized_classes_addr.");
        return DescriptorMapUpdateResult::Fail();
    }
    
    // Make some types for our arguments
    CompilerType clang_uint32_t_type = ast->GetBuiltinTypeForEncodingAndBitSize(eEncodingUint, 32);
    CompilerType clang_void_pointer_type = ast->GetBasicType(eBasicTypeVoid).GetPointerType();
    
    ValueList arguments;
    FunctionCaller *get_shared_cache_class_info_function = nullptr;
    
    if (!m_get_shared_cache_class_info_code.get())
    {
        Error error;
        m_get_shared_cache_class_info_code.reset (GetTargetRef().GetUtilityFunctionForLanguage (g_get_shared_cache_class_info_body,
                                                                                                eLanguageTypeObjC,
                                                                                                g_get_shared_cache_class_info_name,
                                                                                                error));
        if (error.Fail())
        {
            if (log)
                log->Printf ("Failed to get Utility function for implementation lookup: %s.", error.AsCString());
            m_get_shared_cache_class_info_code.reset();
        }
        else
        {
            errors.Clear();
            
            if (!m_get_shared_cache_class_info_code->Install(errors, exe_ctx))
            {
                if (log)
                    log->Printf ("Failed to install implementation lookup: %s.", errors.GetData());
                m_get_shared_cache_class_info_code.reset();
            }
        }
        
        if (!m_get_shared_cache_class_info_code.get())
            return DescriptorMapUpdateResult::Fail();
    
        // Next make the function caller for our implementation utility function.
        Value value;
        value.SetValueType (Value::eValueTypeScalar);
        //value.SetContext (Value::eContextTypeClangType, clang_void_pointer_type);
        value.SetCompilerType (clang_void_pointer_type);
        arguments.PushValue (value);
        arguments.PushValue (value);
        
        value.SetValueType (Value::eValueTypeScalar);
        //value.SetContext (Value::eContextTypeClangType, clang_uint32_t_type);
        value.SetCompilerType (clang_uint32_t_type);
        arguments.PushValue (value);
        
        get_shared_cache_class_info_function = m_get_shared_cache_class_info_code->MakeFunctionCaller(clang_uint32_t_type,
                                                                                                      arguments,
                                                                                                      error);
        
        if (get_shared_cache_class_info_function == nullptr)
            return DescriptorMapUpdateResult::Fail();
    
    }
    else
    {
        get_shared_cache_class_info_function = m_get_shared_cache_class_info_code->GetFunctionCaller();
        if (get_shared_cache_class_info_function == nullptr)
            return DescriptorMapUpdateResult::Fail();
        arguments = get_shared_cache_class_info_function->GetArgumentValues();
    }
    
    errors.Clear();
    
    const uint32_t class_info_byte_size = addr_size + 4;
    const uint32_t class_infos_byte_size = num_classes * class_info_byte_size;
    lldb::addr_t class_infos_addr = process->AllocateMemory (class_infos_byte_size,
                                                             ePermissionsReadable | ePermissionsWritable,
                                                             err);
    
    if (class_infos_addr == LLDB_INVALID_ADDRESS)
        return DescriptorMapUpdateResult::Fail();
    
    Mutex::Locker locker(m_get_shared_cache_class_info_args_mutex);
    
    // Fill in our function argument values
    arguments.GetValueAtIndex(0)->GetScalar() = objc_opt_ptr;
    arguments.GetValueAtIndex(1)->GetScalar() = class_infos_addr;
    arguments.GetValueAtIndex(2)->GetScalar() = class_infos_byte_size;
    
    bool success = false;
    bool any_found = false;
    
    errors.Clear();
    
    // Write our function arguments into the process so we can run our function
    if (get_shared_cache_class_info_function->WriteFunctionArguments (exe_ctx,
                                                                      m_get_shared_cache_class_info_args,
                                                                      arguments,
                                                                      errors))
    {
        EvaluateExpressionOptions options;
        options.SetUnwindOnError(true);
        options.SetTryAllThreads(false);
        options.SetStopOthers(true);
        options.SetIgnoreBreakpoints(true);
        options.SetTimeoutUsec(UTILITY_FUNCTION_TIMEOUT_USEC);
        
        Value return_value;
        return_value.SetValueType (Value::eValueTypeScalar);
        //return_value.SetContext (Value::eContextTypeClangType, clang_uint32_t_type);
        return_value.SetCompilerType (clang_uint32_t_type);
        return_value.GetScalar() = 0;
        
        errors.Clear();
        
        // Run the function
        ExpressionResults results = get_shared_cache_class_info_function->ExecuteFunction (exe_ctx,
                                                                                           &m_get_shared_cache_class_info_args,
                                                                                           options,
                                                                                           errors,
                                                                                           return_value);
        
        if (results == eExpressionCompleted)
        {
            // The result is the number of ClassInfo structures that were filled in
            uint32_t num_class_infos = return_value.GetScalar().ULong();
            if (log)
                log->Printf("Discovered %u ObjC classes in shared cache\n",num_class_infos);
#ifdef LLDB_CONFIGURATION_DEBUG
            assert (num_class_infos <= num_classes);
#endif
            if (num_class_infos > 0)
            {
                if (num_class_infos > num_classes)
                {
                    num_class_infos = num_classes;
                    
                    success = false;
                }
                else
                {
                    success = true;
                }
                
                // Read the ClassInfo structures
                DataBufferHeap buffer (num_class_infos * class_info_byte_size, 0);
                if (process->ReadMemory(class_infos_addr,
                                        buffer.GetBytes(),
                                        buffer.GetByteSize(),
                                        err) == buffer.GetByteSize())
                {
                    DataExtractor class_infos_data (buffer.GetBytes(),
                                                    buffer.GetByteSize(),
                                                    process->GetByteOrder(),
                                                    addr_size);

                    any_found = (ParseClassInfoArray (class_infos_data, num_class_infos) > 0);
                }
            }
            else
            {
                success = true;
            }
        }
        else
        {
            if (log)
                log->Printf("Error evaluating our find class name function: %s.\n", errors.GetData());
        }
    }
    else
    {
        if (log)
            log->Printf ("Error writing function arguments: \"%s\".", errors.GetData());
    }
    
    // Deallocate the memory we allocated for the ClassInfo array
    process->DeallocateMemory(class_infos_addr);
    
    return DescriptorMapUpdateResult(success, any_found);
}

bool
AppleObjCRuntimeV2::UpdateISAToDescriptorMapFromMemory (RemoteNXMapTable &hash_table)
{
    Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_PROCESS));
    
    Process *process = GetProcess();

    if (process == NULL)
        return false;
    
    uint32_t num_map_table_isas = 0;
    
    ModuleSP objc_module_sp(GetObjCModule());
    
    if (objc_module_sp)
    {
        for (RemoteNXMapTable::element elt : hash_table)
        {
            ++num_map_table_isas;
            
            if (ISAIsCached(elt.second))
                continue;
            
            ClassDescriptorSP descriptor_sp = ClassDescriptorSP(new ClassDescriptorV2(*this, elt.second, elt.first.AsCString()));
            
            if (log && log->GetVerbose())
                log->Printf("AppleObjCRuntimeV2 added (ObjCISA)0x%" PRIx64 " (%s) from dynamic table to isa->descriptor cache", elt.second, elt.first.AsCString());
            
            AddClass (elt.second, descriptor_sp, elt.first.AsCString());
        }
    }
    
    return num_map_table_isas > 0;
}

lldb::addr_t
AppleObjCRuntimeV2::GetSharedCacheReadOnlyAddress()
{
    Process *process = GetProcess();
    
    if (process)
    {
        ModuleSP objc_module_sp(GetObjCModule());
        
        if (objc_module_sp)
        {
            ObjectFile *objc_object = objc_module_sp->GetObjectFile();
            
            if (objc_object)
            {
                SectionList *section_list = objc_module_sp->GetSectionList();
                
                if (section_list)
                {
                    SectionSP text_segment_sp (section_list->FindSectionByName(ConstString("__TEXT")));
                    
                    if (text_segment_sp)
                    {
                        SectionSP objc_opt_section_sp (text_segment_sp->GetChildren().FindSectionByName(ConstString("__objc_opt_ro")));
                        
                        if (objc_opt_section_sp)
                        {
                            return objc_opt_section_sp->GetLoadBaseAddress(&process->GetTarget());
                        }
                    }
                }
            }
        }
    }
    return LLDB_INVALID_ADDRESS;
}

void
AppleObjCRuntimeV2::UpdateISAToDescriptorMapIfNeeded()
{
    Timer scoped_timer (__PRETTY_FUNCTION__, __PRETTY_FUNCTION__);
    
    // Else we need to check with our process to see when the map was updated.
    Process *process = GetProcess();

    if (process)
    {
        RemoteNXMapTable hash_table;
        
        // Update the process stop ID that indicates the last time we updated the
        // map, whether it was successful or not.
        m_isa_to_descriptor_stop_id = process->GetStopID();
        
        if (!m_hash_signature.NeedsUpdate(process, this, hash_table))
            return;
        
        m_hash_signature.UpdateSignature (hash_table);

        // Grab the dynamically loaded objc classes from the hash table in memory
        UpdateISAToDescriptorMapDynamic(hash_table);

        // Now get the objc classes that are baked into the Objective C runtime
        // in the shared cache, but only once per process as this data never
        // changes
        if (!m_loaded_objc_opt)
        {
            DescriptorMapUpdateResult shared_cache_update_result = UpdateISAToDescriptorMapSharedCache();
            if (!shared_cache_update_result.any_found)
                WarnIfNoClassesCached ();
            else
                m_loaded_objc_opt = true;
        }
    }
    else
    {
        m_isa_to_descriptor_stop_id = UINT32_MAX;
    }
}

void
AppleObjCRuntimeV2::WarnIfNoClassesCached ()
{
    if (m_noclasses_warning_emitted)
        return;

    if (m_process &&
        m_process->GetTarget().GetPlatform() &&
        m_process->GetTarget().GetPlatform()->GetPluginName().GetStringRef().endswith("-simulator"))
    {
        // Simulators do not have the objc_opt_ro class table so don't actually complain to the user
        m_noclasses_warning_emitted = true;
        return;
    }

    Debugger &debugger(GetProcess()->GetTarget().GetDebugger());
    
    if (debugger.GetAsyncOutputStream())
    {
        debugger.GetAsyncOutputStream()->PutCString("warning: could not load any Objective-C class information from the dyld shared cache. This will significantly reduce the quality of type information available.\n");
        m_noclasses_warning_emitted = true;
    }
}

// TODO: should we have a transparent_kvo parameter here to say if we
// want to replace the KVO swizzled class with the actual user-level type?
ConstString
AppleObjCRuntimeV2::GetActualTypeName(ObjCLanguageRuntime::ObjCISA isa)
{
    if (isa == g_objc_Tagged_ISA)
    {
        static const ConstString g_objc_tagged_isa_name ("_lldb_Tagged_ObjC_ISA");
        return g_objc_tagged_isa_name;
    }
    if (isa == g_objc_Tagged_ISA_NSAtom)
    {
        static const ConstString g_objc_tagged_isa_nsatom_name ("NSAtom");
        return g_objc_tagged_isa_nsatom_name;
    }
    if (isa == g_objc_Tagged_ISA_NSNumber)
    {
        static const ConstString g_objc_tagged_isa_nsnumber_name ("NSNumber");
        return g_objc_tagged_isa_nsnumber_name;
    }
    if (isa == g_objc_Tagged_ISA_NSDateTS)
    {
        static const ConstString g_objc_tagged_isa_nsdatets_name ("NSDateTS");
        return g_objc_tagged_isa_nsdatets_name;
    }
    if (isa == g_objc_Tagged_ISA_NSManagedObject)
    {
        static const ConstString g_objc_tagged_isa_nsmanagedobject_name ("NSManagedObject");
        return g_objc_tagged_isa_nsmanagedobject_name;
    }
    if (isa == g_objc_Tagged_ISA_NSDate)
    {
        static const ConstString g_objc_tagged_isa_nsdate_name ("NSDate");
        return g_objc_tagged_isa_nsdate_name;
    }
    return ObjCLanguageRuntime::GetActualTypeName(isa);
}

DeclVendor *
AppleObjCRuntimeV2::GetDeclVendor()
{
    if (!m_decl_vendor_ap.get())
        m_decl_vendor_ap.reset(new AppleObjCDeclVendor(*this));
    
    return m_decl_vendor_ap.get();
}

lldb::addr_t
AppleObjCRuntimeV2::LookupRuntimeSymbol (const ConstString &name)
{
    lldb::addr_t ret = LLDB_INVALID_ADDRESS;

    const char *name_cstr = name.AsCString();    
    
    if (name_cstr)
    {
        llvm::StringRef name_strref(name_cstr);
        
        static const llvm::StringRef ivar_prefix("OBJC_IVAR_$_");
        static const llvm::StringRef class_prefix("OBJC_CLASS_$_");
        
        if (name_strref.startswith(ivar_prefix))
        {
            llvm::StringRef ivar_skipped_prefix = name_strref.substr(ivar_prefix.size());
            std::pair<llvm::StringRef, llvm::StringRef> class_and_ivar = ivar_skipped_prefix.split('.');
            
            if (class_and_ivar.first.size() && class_and_ivar.second.size())
            {
                const ConstString class_name_cs(class_and_ivar.first);
                ClassDescriptorSP descriptor = ObjCLanguageRuntime::GetClassDescriptorFromClassName(class_name_cs);
                                
                if (descriptor)
                {
                    const ConstString ivar_name_cs(class_and_ivar.second);
                    const char *ivar_name_cstr = ivar_name_cs.AsCString();
                    
                    auto ivar_func = [&ret, ivar_name_cstr](const char *name, const char *type, lldb::addr_t offset_addr, uint64_t size) -> lldb::addr_t
                    {
                        if (!strcmp(name, ivar_name_cstr))
                        {
                            ret = offset_addr;
                            return true;
                        }
                        return false;
                    };

                    descriptor->Describe(std::function<void (ObjCISA)>(nullptr),
                                         std::function<bool (const char *, const char *)>(nullptr),
                                         std::function<bool (const char *, const char *)>(nullptr),
                                         ivar_func);
                }
            }
        }
        else if (name_strref.startswith(class_prefix))
        {
            llvm::StringRef class_skipped_prefix = name_strref.substr(class_prefix.size());
            const ConstString class_name_cs(class_skipped_prefix);
            ClassDescriptorSP descriptor = GetClassDescriptorFromClassName(class_name_cs);
            
            if (descriptor)
                ret = descriptor->GetISA();
        }
    }
    
    return ret;
}

AppleObjCRuntimeV2::NonPointerISACache*
AppleObjCRuntimeV2::NonPointerISACache::CreateInstance (AppleObjCRuntimeV2& runtime, const lldb::ModuleSP& objc_module_sp)
{
    Process* process(runtime.GetProcess());
    
    Error error;
    
    auto objc_debug_isa_magic_mask = ExtractRuntimeGlobalSymbol(process,
                                                                ConstString("objc_debug_isa_magic_mask"),
                                                                objc_module_sp,
                                                                error);
    if (error.Fail())
        return NULL;

    auto objc_debug_isa_magic_value = ExtractRuntimeGlobalSymbol(process,
                                                                 ConstString("objc_debug_isa_magic_value"),
                                                                 objc_module_sp,
                                                                 error);
    if (error.Fail())
        return NULL;

    auto objc_debug_isa_class_mask = ExtractRuntimeGlobalSymbol(process,
                                                                ConstString("objc_debug_isa_class_mask"),
                                                                objc_module_sp,
                                                                error);
    if (error.Fail())
        return NULL;

    // we might want to have some rules to outlaw these other values (e.g if the mask is zero but the value is non-zero, ...)
    
    return new NonPointerISACache(runtime,
                                  objc_debug_isa_class_mask,
                                  objc_debug_isa_magic_mask,
                                  objc_debug_isa_magic_value);
}

AppleObjCRuntimeV2::TaggedPointerVendorV2*
AppleObjCRuntimeV2::TaggedPointerVendorV2::CreateInstance (AppleObjCRuntimeV2& runtime, const lldb::ModuleSP& objc_module_sp)
{
    Process* process(runtime.GetProcess());
    
    Error error;
    
    auto objc_debug_taggedpointer_mask = ExtractRuntimeGlobalSymbol(process,
                                                                    ConstString("objc_debug_taggedpointer_mask"),
                                                                    objc_module_sp,
                                                                    error);
    if (error.Fail())
        return new TaggedPointerVendorLegacy(runtime);
    
    auto objc_debug_taggedpointer_slot_shift = ExtractRuntimeGlobalSymbol(process,
                                                                          ConstString("objc_debug_taggedpointer_slot_shift"),
                                                                          objc_module_sp,
                                                                          error,
                                                                          true,
                                                                          4);
    if (error.Fail())
        return new TaggedPointerVendorLegacy(runtime);
    
    auto objc_debug_taggedpointer_slot_mask = ExtractRuntimeGlobalSymbol(process,
                                                                          ConstString("objc_debug_taggedpointer_slot_mask"),
                                                                          objc_module_sp,
                                                                          error,
                                                                          true,
                                                                          4);
    if (error.Fail())
        return new TaggedPointerVendorLegacy(runtime);

    auto objc_debug_taggedpointer_payload_lshift = ExtractRuntimeGlobalSymbol(process,
                                                                              ConstString("objc_debug_taggedpointer_payload_lshift"),
                                                                              objc_module_sp,
                                                                              error,
                                                                              true,
                                                                              4);
    if (error.Fail())
        return new TaggedPointerVendorLegacy(runtime);
    
    auto objc_debug_taggedpointer_payload_rshift = ExtractRuntimeGlobalSymbol(process,
                                                                              ConstString("objc_debug_taggedpointer_payload_rshift"),
                                                                              objc_module_sp,
                                                                              error,
                                                                              true,
                                                                              4);
    if (error.Fail())
        return new TaggedPointerVendorLegacy(runtime);
    
    auto objc_debug_taggedpointer_classes = ExtractRuntimeGlobalSymbol(process,
                                                                       ConstString("objc_debug_taggedpointer_classes"),
                                                                       objc_module_sp,
                                                                       error,
                                                                       false);
    if (error.Fail())
        return new TaggedPointerVendorLegacy(runtime);

    
    // we might want to have some rules to outlaw these values (e.g if the table's address is zero)
    
    return new TaggedPointerVendorRuntimeAssisted(runtime,
                                                  objc_debug_taggedpointer_mask,
                                                  objc_debug_taggedpointer_slot_shift,
                                                  objc_debug_taggedpointer_slot_mask,
                                                  objc_debug_taggedpointer_payload_lshift,
                                                  objc_debug_taggedpointer_payload_rshift,
                                                  objc_debug_taggedpointer_classes);
}

bool
AppleObjCRuntimeV2::TaggedPointerVendorLegacy::IsPossibleTaggedPointer (lldb::addr_t ptr)
{
    return (ptr & 1);
}

ObjCLanguageRuntime::ClassDescriptorSP
AppleObjCRuntimeV2::TaggedPointerVendorLegacy::GetClassDescriptor (lldb::addr_t ptr)
{
    if (!IsPossibleTaggedPointer(ptr))
        return ObjCLanguageRuntime::ClassDescriptorSP();

    uint32_t foundation_version = m_runtime.GetFoundationVersion();
    
    if (foundation_version == LLDB_INVALID_MODULE_VERSION)
        return ObjCLanguageRuntime::ClassDescriptorSP();
    
    uint64_t class_bits = (ptr & 0xE) >> 1;
    ConstString name;
    
    // TODO: make a table
    if (foundation_version >= 900)
    {
        switch (class_bits)
        {
            case 0:
                name = ConstString("NSAtom");
                break;
            case 3:
                name = ConstString("NSNumber");
                break;
            case 4:
                name = ConstString("NSDateTS");
                break;
            case 5:
                name = ConstString("NSManagedObject");
                break;
            case 6:
                name = ConstString("NSDate");
                break;
            default:
                return ObjCLanguageRuntime::ClassDescriptorSP();
        }
    }
    else
    {
        switch (class_bits)
        {
            case 1:
                name = ConstString("NSNumber");
                break;
            case 5:
                name = ConstString("NSManagedObject");
                break;
            case 6:
                name = ConstString("NSDate");
                break;
            case 7:
                name = ConstString("NSDateTS");
                break;
            default:
                return ObjCLanguageRuntime::ClassDescriptorSP();
        }
    }
    return ClassDescriptorSP(new ClassDescriptorV2Tagged(name,ptr));
}

AppleObjCRuntimeV2::TaggedPointerVendorRuntimeAssisted::TaggedPointerVendorRuntimeAssisted (AppleObjCRuntimeV2& runtime,
                                                                                            uint64_t objc_debug_taggedpointer_mask,
                                                                                            uint32_t objc_debug_taggedpointer_slot_shift,
                                                                                            uint32_t objc_debug_taggedpointer_slot_mask,
                                                                                            uint32_t objc_debug_taggedpointer_payload_lshift,
                                                                                            uint32_t objc_debug_taggedpointer_payload_rshift,
                                                                                            lldb::addr_t objc_debug_taggedpointer_classes) :
TaggedPointerVendorV2(runtime),
m_cache(),
m_objc_debug_taggedpointer_mask(objc_debug_taggedpointer_mask),
m_objc_debug_taggedpointer_slot_shift(objc_debug_taggedpointer_slot_shift),
m_objc_debug_taggedpointer_slot_mask(objc_debug_taggedpointer_slot_mask),
m_objc_debug_taggedpointer_payload_lshift(objc_debug_taggedpointer_payload_lshift),
m_objc_debug_taggedpointer_payload_rshift(objc_debug_taggedpointer_payload_rshift),
m_objc_debug_taggedpointer_classes(objc_debug_taggedpointer_classes)
{
}

bool
AppleObjCRuntimeV2::TaggedPointerVendorRuntimeAssisted::IsPossibleTaggedPointer (lldb::addr_t ptr)
{
    return (ptr & m_objc_debug_taggedpointer_mask) != 0;
}

ObjCLanguageRuntime::ClassDescriptorSP
AppleObjCRuntimeV2::TaggedPointerVendorRuntimeAssisted::GetClassDescriptor (lldb::addr_t ptr)
{
    ClassDescriptorSP actual_class_descriptor_sp;
    uint64_t data_payload;

    if (!IsPossibleTaggedPointer(ptr))
        return ObjCLanguageRuntime::ClassDescriptorSP();
    
    uintptr_t slot = (ptr >> m_objc_debug_taggedpointer_slot_shift) & m_objc_debug_taggedpointer_slot_mask;
    
    CacheIterator iterator = m_cache.find(slot),
    end = m_cache.end();
    if (iterator != end)
    {
        actual_class_descriptor_sp = iterator->second;
    }
    else
    {
        Process* process(m_runtime.GetProcess());
        uintptr_t slot_ptr = slot*process->GetAddressByteSize()+m_objc_debug_taggedpointer_classes;
        Error error;
        uintptr_t slot_data = process->ReadPointerFromMemory(slot_ptr, error);
        if (error.Fail() || slot_data == 0 || slot_data == uintptr_t(LLDB_INVALID_ADDRESS))
            return nullptr;
        actual_class_descriptor_sp = m_runtime.GetClassDescriptorFromISA((ObjCISA)slot_data);
        if (!actual_class_descriptor_sp)
            return ObjCLanguageRuntime::ClassDescriptorSP();
        m_cache[slot] = actual_class_descriptor_sp;
    }
    
    data_payload = (((uint64_t)ptr << m_objc_debug_taggedpointer_payload_lshift) >> m_objc_debug_taggedpointer_payload_rshift);
    
    return ClassDescriptorSP(new ClassDescriptorV2Tagged(actual_class_descriptor_sp,data_payload));
}

AppleObjCRuntimeV2::NonPointerISACache::NonPointerISACache (AppleObjCRuntimeV2& runtime,
                                                            uint64_t objc_debug_isa_class_mask,
                                                            uint64_t objc_debug_isa_magic_mask,
                                                            uint64_t objc_debug_isa_magic_value) :
m_runtime(runtime),
m_cache(),
m_objc_debug_isa_class_mask(objc_debug_isa_class_mask),
m_objc_debug_isa_magic_mask(objc_debug_isa_magic_mask),
m_objc_debug_isa_magic_value(objc_debug_isa_magic_value)
{
}

ObjCLanguageRuntime::ClassDescriptorSP
AppleObjCRuntimeV2::NonPointerISACache::GetClassDescriptor (ObjCISA isa)
{
    ObjCISA real_isa = 0;
    if (EvaluateNonPointerISA(isa, real_isa) == false)
        return ObjCLanguageRuntime::ClassDescriptorSP();
    auto cache_iter = m_cache.find(real_isa);
    if (cache_iter != m_cache.end())
        return cache_iter->second;
    auto descriptor_sp = m_runtime.ObjCLanguageRuntime::GetClassDescriptorFromISA(real_isa);
    if (descriptor_sp) // cache only positive matches since the table might grow
        m_cache[real_isa] = descriptor_sp;
    return descriptor_sp;
}

bool
AppleObjCRuntimeV2::NonPointerISACache::EvaluateNonPointerISA (ObjCISA isa, ObjCISA& ret_isa)
{
    if ( (isa & ~m_objc_debug_isa_class_mask) == 0)
        return false;
    if ( (isa & m_objc_debug_isa_magic_mask) == m_objc_debug_isa_magic_value)
    {
        ret_isa = isa & m_objc_debug_isa_class_mask;
        return (ret_isa != 0); // this is a pointer so 0 is not a valid value
    }
    return false;
}

ObjCLanguageRuntime::EncodingToTypeSP
AppleObjCRuntimeV2::GetEncodingToType ()
{
    if (!m_encoding_to_type_sp)
        m_encoding_to_type_sp.reset(new AppleObjCTypeEncodingParser(*this));
    return m_encoding_to_type_sp;
}

lldb_private::AppleObjCRuntime::ObjCISA
AppleObjCRuntimeV2::GetPointerISA (ObjCISA isa)
{
    ObjCISA ret = isa;
    
    if (m_non_pointer_isa_cache_ap)
        m_non_pointer_isa_cache_ap->EvaluateNonPointerISA(isa, ret);
    
    return ret;
}
