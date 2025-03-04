//===-- SymbolFileDWARF.cpp ------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "SymbolFileDWARF.h"

// Other libraries and framework includes
#include "llvm/Support/Casting.h"

#include "lldb/Core/ArchSpec.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/ModuleList.h"
#include "lldb/Core/ModuleSpec.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Core/RegularExpression.h"
#include "lldb/Core/Scalar.h"
#include "lldb/Core/Section.h"
#include "lldb/Core/StreamFile.h"
#include "lldb/Core/StreamString.h"
#include "lldb/Core/Timer.h"
#include "lldb/Core/Value.h"

#include "Plugins/ExpressionParser/Clang/ClangModulesDeclVendor.h"

#include "lldb/Host/FileSystem.h"
#include "lldb/Host/Host.h"

#include "lldb/Interpreter/OptionValueFileSpecList.h"
#include "lldb/Interpreter/OptionValueProperties.h"

#include "lldb/Symbol/Block.h"
#include "lldb/Symbol/ClangASTContext.h"
#include "lldb/Symbol/CompilerDecl.h"
#include "lldb/Symbol/CompilerDeclContext.h"
#include "lldb/Symbol/CompileUnit.h"
#include "lldb/Symbol/LineTable.h"
#include "lldb/Symbol/DebugMacros.h"
#include "lldb/Symbol/ObjectFile.h"
#include "lldb/Symbol/SymbolVendor.h"
#include "lldb/Symbol/TypeSystem.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Symbol/TypeMap.h"

#include "Plugins/Language/CPlusPlus/CPlusPlusLanguage.h"
#include "Plugins/Language/ObjC/ObjCLanguage.h"

#include "lldb/Target/Language.h"

#include "lldb/Utility/TaskPool.h"

#include "DWARFASTParser.h"
#include "DWARFCompileUnit.h"
#include "DWARFDebugAbbrev.h"
#include "DWARFDebugAranges.h"
#include "DWARFDebugInfo.h"
#include "DWARFDebugLine.h"
#include "DWARFDebugMacro.h"
#include "DWARFDebugPubnames.h"
#include "DWARFDebugRanges.h"
#include "DWARFDeclContext.h"
#include "DWARFDIECollection.h"
#include "DWARFFormValue.h"
#include "LogChannelDWARF.h"
#include "SymbolFileDWARFDwo.h"
#include "SymbolFileDWARFDebugMap.h"

#include <map>

#include <ctype.h>
#include <string.h>

//#define ENABLE_DEBUG_PRINTF // COMMENT OUT THIS LINE PRIOR TO CHECKIN

#ifdef ENABLE_DEBUG_PRINTF
#include <stdio.h>
#define DEBUG_PRINTF(fmt, ...) printf(fmt, __VA_ARGS__)
#else
#define DEBUG_PRINTF(fmt, ...)
#endif

using namespace lldb;
using namespace lldb_private;

//static inline bool
//child_requires_parent_class_union_or_struct_to_be_completed (dw_tag_t tag)
//{
//    switch (tag)
//    {
//    default:
//        break;
//    case DW_TAG_subprogram:
//    case DW_TAG_inlined_subroutine:
//    case DW_TAG_class_type:
//    case DW_TAG_structure_type:
//    case DW_TAG_union_type:
//        return true;
//    }
//    return false;
//}
//

namespace {

    PropertyDefinition
    g_properties[] =
    {
        { "comp-dir-symlink-paths" , OptionValue::eTypeFileSpecList, true,  0 ,   nullptr, nullptr, "If the DW_AT_comp_dir matches any of these paths the symbolic links will be resolved at DWARF parse time." },
        {  nullptr                 , OptionValue::eTypeInvalid     , false, 0,    nullptr, nullptr, nullptr }
    };

    enum
    {
        ePropertySymLinkPaths
    };


    class PluginProperties : public Properties
    {
    public:
        static ConstString
        GetSettingName()
        {
            return SymbolFileDWARF::GetPluginNameStatic();
        }

        PluginProperties()
        {
            m_collection_sp.reset (new OptionValueProperties(GetSettingName()));
            m_collection_sp->Initialize(g_properties);
        }

        FileSpecList&
        GetSymLinkPaths()
        {
            OptionValueFileSpecList *option_value = m_collection_sp->GetPropertyAtIndexAsOptionValueFileSpecList(nullptr, true, ePropertySymLinkPaths);
            assert(option_value);
            return option_value->GetCurrentValue();
        }

    };

    typedef std::shared_ptr<PluginProperties> SymbolFileDWARFPropertiesSP;

    static const SymbolFileDWARFPropertiesSP&
    GetGlobalPluginProperties()
    {
        static const auto g_settings_sp(std::make_shared<PluginProperties>());
        return g_settings_sp;
    }

}  // anonymous namespace end


static const char*
removeHostnameFromPathname(const char* path_from_dwarf)
{
    if (!path_from_dwarf || !path_from_dwarf[0])
    {
        return path_from_dwarf;
    }
    
    const char *colon_pos = strchr(path_from_dwarf, ':');
    if (nullptr == colon_pos)
    {
        return path_from_dwarf;
    }
    
    const char *slash_pos = strchr(path_from_dwarf, '/');
    if (slash_pos && (slash_pos < colon_pos))
    {
        return path_from_dwarf;
    }
    
    // check whether we have a windows path, and so the first character
    // is a drive-letter not a hostname.
    if (
        colon_pos == path_from_dwarf + 1 &&
        isalpha(*path_from_dwarf) &&
        strlen(path_from_dwarf) > 2 &&
        '\\' == path_from_dwarf[2])
    {
        return path_from_dwarf;
    }
    
    return colon_pos + 1;
}

static const char*
resolveCompDir(const char* path_from_dwarf)
{
    if (!path_from_dwarf)
        return nullptr;

    // DWARF2/3 suggests the form hostname:pathname for compilation directory.
    // Remove the host part if present.
    const char* local_path = removeHostnameFromPathname(path_from_dwarf);
    if (!local_path)
        return nullptr;

    bool is_symlink = false;
    FileSpec local_path_spec(local_path, false);
    const auto& file_specs = GetGlobalPluginProperties()->GetSymLinkPaths();
    for (size_t i = 0; i < file_specs.GetSize() && !is_symlink; ++i)
        is_symlink = FileSpec::Equal(file_specs.GetFileSpecAtIndex(i), local_path_spec, true);

    if (!is_symlink)
        return local_path;

    if (!local_path_spec.IsSymbolicLink())
        return local_path;

    FileSpec resolved_local_path_spec;
    const auto error = FileSystem::Readlink(local_path_spec, resolved_local_path_spec);
    if (error.Success())
        return resolved_local_path_spec.GetCString();

    return nullptr;
}


void
SymbolFileDWARF::Initialize()
{
    LogChannelDWARF::Initialize();
    PluginManager::RegisterPlugin (GetPluginNameStatic(),
                                   GetPluginDescriptionStatic(),
                                   CreateInstance,
                                   DebuggerInitialize);
}

void
SymbolFileDWARF::DebuggerInitialize(Debugger &debugger)
{
    if (!PluginManager::GetSettingForSymbolFilePlugin(debugger, PluginProperties::GetSettingName()))
    {
        const bool is_global_setting = true;
        PluginManager::CreateSettingForSymbolFilePlugin(debugger,
                                                        GetGlobalPluginProperties()->GetValueProperties(),
                                                        ConstString ("Properties for the dwarf symbol-file plug-in."),
                                                        is_global_setting);
    }
}

void
SymbolFileDWARF::Terminate()
{
    PluginManager::UnregisterPlugin (CreateInstance);
    LogChannelDWARF::Initialize();
}


lldb_private::ConstString
SymbolFileDWARF::GetPluginNameStatic()
{
    static ConstString g_name("dwarf");
    return g_name;
}

const char *
SymbolFileDWARF::GetPluginDescriptionStatic()
{
    return "DWARF and DWARF3 debug symbol file reader.";
}


SymbolFile*
SymbolFileDWARF::CreateInstance (ObjectFile* obj_file)
{
    return new SymbolFileDWARF(obj_file);
}

TypeList *          
SymbolFileDWARF::GetTypeList ()
{
    if (GetDebugMapSymfile ())
        return m_debug_map_symfile->GetTypeList();
    return m_obj_file->GetModule()->GetTypeList();

}
void
SymbolFileDWARF::GetTypes (const DWARFDIE &die,
                           dw_offset_t min_die_offset,
                           dw_offset_t max_die_offset,
                           uint32_t type_mask,
                           TypeSet &type_set)
{
    if (die)
    {
        const dw_offset_t die_offset = die.GetOffset();
        
        if (die_offset >= max_die_offset)
            return;
        
        if (die_offset >= min_die_offset)
        {
            const dw_tag_t tag = die.Tag();
            
            bool add_type = false;

            switch (tag)
            {
                case DW_TAG_array_type:         add_type = (type_mask & eTypeClassArray         ) != 0; break;
                case DW_TAG_unspecified_type:
                case DW_TAG_base_type:          add_type = (type_mask & eTypeClassBuiltin       ) != 0; break;
                case DW_TAG_class_type:         add_type = (type_mask & eTypeClassClass         ) != 0; break;
                case DW_TAG_structure_type:     add_type = (type_mask & eTypeClassStruct        ) != 0; break;
                case DW_TAG_union_type:         add_type = (type_mask & eTypeClassUnion         ) != 0; break;
                case DW_TAG_enumeration_type:   add_type = (type_mask & eTypeClassEnumeration   ) != 0; break;
                case DW_TAG_subroutine_type:
                case DW_TAG_subprogram:
                case DW_TAG_inlined_subroutine: add_type = (type_mask & eTypeClassFunction      ) != 0; break;
                case DW_TAG_pointer_type:       add_type = (type_mask & eTypeClassPointer       ) != 0; break;
                case DW_TAG_rvalue_reference_type:
                case DW_TAG_reference_type:     add_type = (type_mask & eTypeClassReference     ) != 0; break;
                case DW_TAG_typedef:            add_type = (type_mask & eTypeClassTypedef       ) != 0; break;
                case DW_TAG_ptr_to_member_type: add_type = (type_mask & eTypeClassMemberPointer ) != 0; break;
            }

            if (add_type)
            {
                const bool assert_not_being_parsed = true;
                Type *type = ResolveTypeUID (die, assert_not_being_parsed);
                if (type)
                {
                    if (type_set.find(type) == type_set.end())
                        type_set.insert(type);
                }
            }
        }
        
        for (DWARFDIE child_die = die.GetFirstChild();
             child_die.IsValid();
             child_die = child_die.GetSibling())
        {
            GetTypes (child_die, min_die_offset, max_die_offset, type_mask, type_set);
        }
    }
}

size_t
SymbolFileDWARF::GetTypes (SymbolContextScope *sc_scope,
                           uint32_t type_mask,
                           TypeList &type_list)

{
    TypeSet type_set;
    
    CompileUnit *comp_unit = NULL;
    DWARFCompileUnit* dwarf_cu = NULL;
    if (sc_scope)
        comp_unit = sc_scope->CalculateSymbolContextCompileUnit();

    if (comp_unit)
    {
        dwarf_cu = GetDWARFCompileUnit(comp_unit);
        if (dwarf_cu == 0)
            return 0;
        GetTypes (dwarf_cu->DIE(),
                  dwarf_cu->GetOffset(),
                  dwarf_cu->GetNextCompileUnitOffset(),
                  type_mask,
                  type_set);
    }
    else
    {
        DWARFDebugInfo* info = DebugInfo();
        if (info)
        {
            const size_t num_cus = info->GetNumCompileUnits();
            for (size_t cu_idx=0; cu_idx<num_cus; ++cu_idx)
            {
                dwarf_cu = info->GetCompileUnitAtIndex(cu_idx);
                if (dwarf_cu)
                {
                    GetTypes (dwarf_cu->DIE(),
                              0,
                              UINT32_MAX,
                              type_mask,
                              type_set);
                }
            }
        }
    }

    std::set<CompilerType> compiler_type_set;
    size_t num_types_added = 0;
    for (Type *type : type_set)
    {
        CompilerType compiler_type = type->GetForwardCompilerType ();
        if (compiler_type_set.find(compiler_type) == compiler_type_set.end())
        {
            compiler_type_set.insert(compiler_type);
            type_list.Insert (type->shared_from_this());
            ++num_types_added;
        }
    }
    return num_types_added;
}


//----------------------------------------------------------------------
// Gets the first parent that is a lexical block, function or inlined
// subroutine, or compile unit.
//----------------------------------------------------------------------
DWARFDIE
SymbolFileDWARF::GetParentSymbolContextDIE(const DWARFDIE &child_die)
{
    DWARFDIE die;
    for (die = child_die.GetParent(); die; die = die.GetParent())
    {
        dw_tag_t tag = die.Tag();

        switch (tag)
        {
        case DW_TAG_compile_unit:
        case DW_TAG_subprogram:
        case DW_TAG_inlined_subroutine:
        case DW_TAG_lexical_block:
            return die;
        }
    }
    return DWARFDIE();
}


SymbolFileDWARF::SymbolFileDWARF(ObjectFile* objfile) :
    SymbolFile (objfile),
    UserID (0),  // Used by SymbolFileDWARFDebugMap to when this class parses .o files to contain the .o file index/ID
    m_debug_map_module_wp (),
    m_debug_map_symfile (NULL),
    m_data_debug_abbrev (),
    m_data_debug_aranges (),
    m_data_debug_frame (),
    m_data_debug_info (),
    m_data_debug_line (),
    m_data_debug_macro (),
    m_data_debug_loc (),
    m_data_debug_ranges (),
    m_data_debug_str (),
    m_data_apple_names (),
    m_data_apple_types (),
    m_data_apple_namespaces (),
    m_abbr(),
    m_info(),
    m_line(),
    m_apple_names_ap (),
    m_apple_types_ap (),
    m_apple_namespaces_ap (),
    m_apple_objc_ap (),
    m_function_basename_index(),
    m_function_fullname_index(),
    m_function_method_index(),
    m_function_selector_index(),
    m_objc_class_selectors_index(),
    m_global_index(),
    m_type_index(),
    m_namespace_index(),
    m_indexed (false),
    m_using_apple_tables (false),
    m_fetched_external_modules (false),
    m_supports_DW_AT_APPLE_objc_complete_type (eLazyBoolCalculate),
    m_ranges(),
    m_unique_ast_type_map ()
{
}

SymbolFileDWARF::~SymbolFileDWARF()
{
}

static const ConstString &
GetDWARFMachOSegmentName ()
{
    static ConstString g_dwarf_section_name ("__DWARF");
    return g_dwarf_section_name;
}

UniqueDWARFASTTypeMap &
SymbolFileDWARF::GetUniqueDWARFASTTypeMap ()
{
    if (GetDebugMapSymfile ())
        return m_debug_map_symfile->GetUniqueDWARFASTTypeMap ();
    return m_unique_ast_type_map;
}

TypeSystem *
SymbolFileDWARF::GetTypeSystemForLanguage (LanguageType language)
{
    SymbolFileDWARFDebugMap * debug_map_symfile = GetDebugMapSymfile ();
    TypeSystem *type_system;
    if (debug_map_symfile)
    {
        type_system = debug_map_symfile->GetTypeSystemForLanguage(language);
    }
    else
    {
        type_system = m_obj_file->GetModule()->GetTypeSystemForLanguage(language);
        if (type_system)
            type_system->SetSymbolFile(this);
    }
    return type_system;
}

void
SymbolFileDWARF::InitializeObject()
{
    ModuleSP module_sp (m_obj_file->GetModule());
    if (module_sp)
    {
        const SectionList *section_list = module_sp->GetSectionList();
        const Section* section = section_list->FindSectionByName(GetDWARFMachOSegmentName ()).get();

        // Memory map the DWARF mach-o segment so we have everything mmap'ed
        // to keep our heap memory usage down.
        if (section)
            m_obj_file->MemoryMapSectionData(section, m_dwarf_data);
    }

    get_apple_names_data();
    if (m_data_apple_names.m_data.GetByteSize() > 0)
    {
        m_apple_names_ap.reset (new DWARFMappedHash::MemoryTable (m_data_apple_names.m_data,
                                                                  get_debug_str_data(),
                                                                  ".apple_names"));
        if (m_apple_names_ap->IsValid())
            m_using_apple_tables = true;
        else
            m_apple_names_ap.reset();
    }
    get_apple_types_data();
    if (m_data_apple_types.m_data.GetByteSize() > 0)
    {
        m_apple_types_ap.reset (new DWARFMappedHash::MemoryTable (m_data_apple_types.m_data,
                                                                  get_debug_str_data(),
                                                                  ".apple_types"));
        if (m_apple_types_ap->IsValid())
            m_using_apple_tables = true;
        else
            m_apple_types_ap.reset();
    }

    get_apple_namespaces_data();
    if (m_data_apple_namespaces.m_data.GetByteSize() > 0)
    {
        m_apple_namespaces_ap.reset (new DWARFMappedHash::MemoryTable (m_data_apple_namespaces.m_data,
                                                                       get_debug_str_data(),
                                                                       ".apple_namespaces"));
        if (m_apple_namespaces_ap->IsValid())
            m_using_apple_tables = true;
        else
            m_apple_namespaces_ap.reset();
    }

    get_apple_objc_data();
    if (m_data_apple_objc.m_data.GetByteSize() > 0)
    {
        m_apple_objc_ap.reset (new DWARFMappedHash::MemoryTable (m_data_apple_objc.m_data,
                                                                 get_debug_str_data(),
                                                                 ".apple_objc"));
        if (m_apple_objc_ap->IsValid())
            m_using_apple_tables = true;
        else
            m_apple_objc_ap.reset();
    }
}

bool
SymbolFileDWARF::SupportedVersion(uint16_t version)
{
    return version == 2 || version == 3 || version == 4;
}

uint32_t
SymbolFileDWARF::CalculateAbilities ()
{
    uint32_t abilities = 0;
    if (m_obj_file != NULL)
    {
        const Section* section = NULL;
        const SectionList *section_list = m_obj_file->GetSectionList();
        if (section_list == NULL)
            return 0;

        uint64_t debug_abbrev_file_size = 0;
        uint64_t debug_info_file_size = 0;
        uint64_t debug_line_file_size = 0;

        section = section_list->FindSectionByName(GetDWARFMachOSegmentName ()).get();
        
        if (section)
            section_list = &section->GetChildren ();
        
        section = section_list->FindSectionByType (eSectionTypeDWARFDebugInfo, true).get();
        if (section != NULL)
        {
            debug_info_file_size = section->GetFileSize();

            section = section_list->FindSectionByType (eSectionTypeDWARFDebugAbbrev, true).get();
            if (section)
                debug_abbrev_file_size = section->GetFileSize();

            section = section_list->FindSectionByType (eSectionTypeDWARFDebugLine, true).get();
            if (section)
                debug_line_file_size = section->GetFileSize();
        }
        else
        {
            const char *symfile_dir_cstr = m_obj_file->GetFileSpec().GetDirectory().GetCString();
            if (symfile_dir_cstr)
            {
                if (strcasestr(symfile_dir_cstr, ".dsym"))
                {
                    if (m_obj_file->GetType() == ObjectFile::eTypeDebugInfo)
                    {
                        // We have a dSYM file that didn't have a any debug info.
                        // If the string table has a size of 1, then it was made from
                        // an executable with no debug info, or from an executable that
                        // was stripped.
                        section = section_list->FindSectionByType (eSectionTypeDWARFDebugStr, true).get();
                        if (section && section->GetFileSize() == 1)
                        {
                            m_obj_file->GetModule()->ReportWarning ("empty dSYM file detected, dSYM was created with an executable with no debug info.");
                        }
                    }
                }
            }
        }

        if (debug_abbrev_file_size > 0 && debug_info_file_size > 0)
            abilities |= CompileUnits | Functions | Blocks | GlobalVariables | LocalVariables | VariableTypes;

        if (debug_line_file_size > 0)
            abilities |= LineTables;
    }
    return abilities;
}

const DWARFDataExtractor&
SymbolFileDWARF::GetCachedSectionData (lldb::SectionType sect_type, DWARFDataSegment& data_segment)
{
    std::call_once(data_segment.m_flag,
                   &SymbolFileDWARF::LoadSectionData,
                   this,
                   sect_type,
                   std::ref(data_segment.m_data));
    return data_segment.m_data;
}

void
SymbolFileDWARF::LoadSectionData (lldb::SectionType sect_type, DWARFDataExtractor& data)
{
    ModuleSP module_sp (m_obj_file->GetModule());
    const SectionList *section_list = module_sp->GetSectionList();
    if (section_list)
    {
        SectionSP section_sp (section_list->FindSectionByType(sect_type, true));
        if (section_sp)
        {
            // See if we memory mapped the DWARF segment?
            if (m_dwarf_data.GetByteSize())
            {
                data.SetData(m_dwarf_data, section_sp->GetOffset(), section_sp->GetFileSize());
            }
            else
            {
                if (m_obj_file->ReadSectionData(section_sp.get(), data) == 0)
                    data.Clear();
            }
        }
    }
}

const DWARFDataExtractor&
SymbolFileDWARF::get_debug_abbrev_data()
{
    return GetCachedSectionData (eSectionTypeDWARFDebugAbbrev, m_data_debug_abbrev);
}

const DWARFDataExtractor&
SymbolFileDWARF::get_debug_addr_data()
{
    return GetCachedSectionData (eSectionTypeDWARFDebugAddr, m_data_debug_addr);
}

const DWARFDataExtractor&
SymbolFileDWARF::get_debug_aranges_data()
{
    return GetCachedSectionData (eSectionTypeDWARFDebugAranges, m_data_debug_aranges);
}

const DWARFDataExtractor&
SymbolFileDWARF::get_debug_frame_data()
{
    return GetCachedSectionData (eSectionTypeDWARFDebugFrame, m_data_debug_frame);
}

const DWARFDataExtractor&
SymbolFileDWARF::get_debug_info_data()
{
    return GetCachedSectionData (eSectionTypeDWARFDebugInfo, m_data_debug_info);
}

const DWARFDataExtractor&
SymbolFileDWARF::get_debug_line_data()
{
    return GetCachedSectionData (eSectionTypeDWARFDebugLine, m_data_debug_line);
}

const DWARFDataExtractor&
SymbolFileDWARF::get_debug_macro_data()
{
    return GetCachedSectionData (eSectionTypeDWARFDebugMacro, m_data_debug_macro);
}

const DWARFDataExtractor&
SymbolFileDWARF::get_debug_loc_data()
{
    return GetCachedSectionData (eSectionTypeDWARFDebugLoc, m_data_debug_loc);
}

const DWARFDataExtractor&
SymbolFileDWARF::get_debug_ranges_data()
{
    return GetCachedSectionData (eSectionTypeDWARFDebugRanges, m_data_debug_ranges);
}

const DWARFDataExtractor&
SymbolFileDWARF::get_debug_str_data()
{
    return GetCachedSectionData (eSectionTypeDWARFDebugStr, m_data_debug_str);
}

const DWARFDataExtractor&
SymbolFileDWARF::get_debug_str_offsets_data()
{
    return GetCachedSectionData (eSectionTypeDWARFDebugStrOffsets, m_data_debug_str_offsets);
}

const DWARFDataExtractor&
SymbolFileDWARF::get_apple_names_data()
{
    return GetCachedSectionData (eSectionTypeDWARFAppleNames, m_data_apple_names);
}

const DWARFDataExtractor&
SymbolFileDWARF::get_apple_types_data()
{
    return GetCachedSectionData (eSectionTypeDWARFAppleTypes, m_data_apple_types);
}

const DWARFDataExtractor&
SymbolFileDWARF::get_apple_namespaces_data()
{
    return GetCachedSectionData (eSectionTypeDWARFAppleNamespaces, m_data_apple_namespaces);
}

const DWARFDataExtractor&
SymbolFileDWARF::get_apple_objc_data()
{
    return GetCachedSectionData (eSectionTypeDWARFAppleObjC, m_data_apple_objc);
}


DWARFDebugAbbrev*
SymbolFileDWARF::DebugAbbrev()
{
    if (m_abbr.get() == NULL)
    {
        const DWARFDataExtractor &debug_abbrev_data = get_debug_abbrev_data();
        if (debug_abbrev_data.GetByteSize() > 0)
        {
            m_abbr.reset(new DWARFDebugAbbrev());
            if (m_abbr.get())
                m_abbr->Parse(debug_abbrev_data);
        }
    }
    return m_abbr.get();
}

const DWARFDebugAbbrev*
SymbolFileDWARF::DebugAbbrev() const
{
    return m_abbr.get();
}


DWARFDebugInfo*
SymbolFileDWARF::DebugInfo()
{
    if (m_info.get() == NULL)
    {
        Timer scoped_timer(__PRETTY_FUNCTION__, "%s this = %p",
                           __PRETTY_FUNCTION__, static_cast<void*>(this));
        if (get_debug_info_data().GetByteSize() > 0)
        {
            m_info.reset(new DWARFDebugInfo());
            if (m_info.get())
            {
                m_info->SetDwarfData(this);
            }
        }
    }
    return m_info.get();
}

const DWARFDebugInfo*
SymbolFileDWARF::DebugInfo() const
{
    return m_info.get();
}

DWARFCompileUnit*
SymbolFileDWARF::GetDWARFCompileUnit(lldb_private::CompileUnit *comp_unit)
{
    if (!comp_unit)
        return nullptr;

    DWARFDebugInfo* info = DebugInfo();
    if (info)
    {
        if (GetDebugMapSymfile ())
        {
            // The debug map symbol file made the compile units for this DWARF
            // file which is .o file with DWARF in it, and we should have
            // only 1 compile unit which is at offset zero in the DWARF.
            // TODO: modify to support LTO .o files where each .o file might
            // have multiple DW_TAG_compile_unit tags.
            
            DWARFCompileUnit *dwarf_cu = info->GetCompileUnit(0);
            if (dwarf_cu && dwarf_cu->GetUserData() == NULL)
                dwarf_cu->SetUserData(comp_unit);
            return dwarf_cu;
        }
        else
        {
            // Just a normal DWARF file whose user ID for the compile unit is
            // the DWARF offset itself

            DWARFCompileUnit *dwarf_cu = info->GetCompileUnit((dw_offset_t)comp_unit->GetID());
            if (dwarf_cu && dwarf_cu->GetUserData() == NULL)
                dwarf_cu->SetUserData(comp_unit);
            return dwarf_cu;

        }
    }
    return NULL;
}


DWARFDebugRanges*
SymbolFileDWARF::DebugRanges()
{
    if (m_ranges.get() == NULL)
    {
        Timer scoped_timer(__PRETTY_FUNCTION__, "%s this = %p",
                           __PRETTY_FUNCTION__, static_cast<void*>(this));
        if (get_debug_ranges_data().GetByteSize() > 0)
        {
            m_ranges.reset(new DWARFDebugRanges());
            if (m_ranges.get())
                m_ranges->Extract(this);
        }
    }
    return m_ranges.get();
}

const DWARFDebugRanges*
SymbolFileDWARF::DebugRanges() const
{
    return m_ranges.get();
}

lldb::CompUnitSP
SymbolFileDWARF::ParseCompileUnit (DWARFCompileUnit* dwarf_cu, uint32_t cu_idx)
{
    CompUnitSP cu_sp;
    if (dwarf_cu)
    {
        CompileUnit *comp_unit = (CompileUnit*)dwarf_cu->GetUserData();
        if (comp_unit)
        {
            // We already parsed this compile unit, had out a shared pointer to it
            cu_sp = comp_unit->shared_from_this();
        }
        else
        {
            if (dwarf_cu->GetSymbolFileDWARF() != this)
            {
                return dwarf_cu->GetSymbolFileDWARF()->ParseCompileUnit(dwarf_cu, cu_idx);
            }
            else if (GetDebugMapSymfile ())
            {
                // Let the debug map create the compile unit
                cu_sp = m_debug_map_symfile->GetCompileUnit(this);
                dwarf_cu->SetUserData(cu_sp.get());
            }
            else
            {
                ModuleSP module_sp (m_obj_file->GetModule());
                if (module_sp)
                {
                    const DWARFDIE cu_die = dwarf_cu->GetCompileUnitDIEOnly ();
                    if (cu_die)
                    {
                        FileSpec cu_file_spec{cu_die.GetName(), false};
                        if (cu_file_spec)
                        {
                            // If we have a full path to the compile unit, we don't need to resolve
                            // the file.  This can be expensive e.g. when the source files are NFS mounted.
                            if (cu_file_spec.IsRelative())
                            {
                                const char *cu_comp_dir{cu_die.GetAttributeValueAsString(DW_AT_comp_dir, nullptr)};
                                cu_file_spec.PrependPathComponent(resolveCompDir(cu_comp_dir));
                            }

                            std::string remapped_file;
                            if (module_sp->RemapSourceFile(cu_file_spec.GetCString(), remapped_file))
                                cu_file_spec.SetFile(remapped_file, false);
                        }

                        LanguageType cu_language = DWARFCompileUnit::LanguageTypeFromDWARF(cu_die.GetAttributeValueAsUnsigned(DW_AT_language, 0));

                        bool is_optimized = dwarf_cu->GetIsOptimized ();
                        cu_sp.reset(new CompileUnit (module_sp,
                                                     dwarf_cu,
                                                     cu_file_spec, 
                                                     dwarf_cu->GetID(),
                                                     cu_language,
                                                     is_optimized));
                        if (cu_sp)
                        {
                            // If we just created a compile unit with an invalid file spec, try and get the
                            // first entry in the supports files from the line table as that should be the
                            // compile unit.
                            if (!cu_file_spec)
                            {
                                cu_file_spec = cu_sp->GetSupportFiles().GetFileSpecAtIndex(1);
                                if (cu_file_spec)
                                {
                                    (FileSpec &)(*cu_sp) = cu_file_spec;
                                    // Also fix the invalid file spec which was copied from the compile unit.
                                    cu_sp->GetSupportFiles().Replace(0, cu_file_spec);
                                }
                            }

                            dwarf_cu->SetUserData(cu_sp.get());
                            
                            // Figure out the compile unit index if we weren't given one
                            if (cu_idx == UINT32_MAX)
                                DebugInfo()->GetCompileUnit(dwarf_cu->GetOffset(), &cu_idx);
                            
                            m_obj_file->GetModule()->GetSymbolVendor()->SetCompileUnitAtIndex(cu_idx, cu_sp);
                        }
                    }
                }
            }
        }
    }
    return cu_sp;
}

uint32_t
SymbolFileDWARF::GetNumCompileUnits()
{
    DWARFDebugInfo* info = DebugInfo();
    if (info)
        return info->GetNumCompileUnits();
    return 0;
}

CompUnitSP
SymbolFileDWARF::ParseCompileUnitAtIndex(uint32_t cu_idx)
{
    CompUnitSP cu_sp;
    DWARFDebugInfo* info = DebugInfo();
    if (info)
    {
        DWARFCompileUnit* dwarf_cu = info->GetCompileUnitAtIndex(cu_idx);
        if (dwarf_cu)
            cu_sp = ParseCompileUnit(dwarf_cu, cu_idx);
    }
    return cu_sp;
}

Function *
SymbolFileDWARF::ParseCompileUnitFunction (const SymbolContext& sc, const DWARFDIE &die)
{
    if (die.IsValid())
    {
        TypeSystem *type_system = GetTypeSystemForLanguage(die.GetCU()->GetLanguageType());

        if (type_system)
        {
            DWARFASTParser *dwarf_ast = type_system->GetDWARFParser();
            if (dwarf_ast)
                return dwarf_ast->ParseFunctionFromDWARF(sc, die);
        }
    }
    return nullptr;
}

bool
SymbolFileDWARF::FixupAddress (Address &addr)
{
    SymbolFileDWARFDebugMap * debug_map_symfile = GetDebugMapSymfile ();
    if (debug_map_symfile)
    {
        return debug_map_symfile->LinkOSOAddress(addr);
    }
    // This is a normal DWARF file, no address fixups need to happen
    return true;
}
lldb::LanguageType
SymbolFileDWARF::ParseCompileUnitLanguage (const SymbolContext& sc)
{
    assert (sc.comp_unit);
    DWARFCompileUnit* dwarf_cu = GetDWARFCompileUnit(sc.comp_unit);
    if (dwarf_cu)
        return dwarf_cu->GetLanguageType();
    else
        return eLanguageTypeUnknown;
}

size_t
SymbolFileDWARF::ParseCompileUnitFunctions(const SymbolContext &sc)
{
    assert (sc.comp_unit);
    size_t functions_added = 0;
    DWARFCompileUnit* dwarf_cu = GetDWARFCompileUnit(sc.comp_unit);
    if (dwarf_cu)
    {
        DWARFDIECollection function_dies;
        const size_t num_functions = dwarf_cu->AppendDIEsWithTag (DW_TAG_subprogram, function_dies);
        size_t func_idx;
        for (func_idx = 0; func_idx < num_functions; ++func_idx)
        {
            DWARFDIE die = function_dies.GetDIEAtIndex(func_idx);
            if (sc.comp_unit->FindFunctionByUID (die.GetID()).get() == NULL)
            {
                if (ParseCompileUnitFunction(sc, die))
                    ++functions_added;
            }
        }
        //FixupTypes();
    }
    return functions_added;
}

bool
SymbolFileDWARF::ParseCompileUnitSupportFiles (const SymbolContext& sc, FileSpecList& support_files)
{
    assert (sc.comp_unit);
    DWARFCompileUnit* dwarf_cu = GetDWARFCompileUnit(sc.comp_unit);
    if (dwarf_cu)
    {
        const DWARFDIE cu_die = dwarf_cu->GetCompileUnitDIEOnly();

        if (cu_die)
        {
            const char * cu_comp_dir = resolveCompDir(cu_die.GetAttributeValueAsString(DW_AT_comp_dir, nullptr));
            const dw_offset_t stmt_list = cu_die.GetAttributeValueAsUnsigned(DW_AT_stmt_list, DW_INVALID_OFFSET);
            if (stmt_list != DW_INVALID_OFFSET)
            {
                // All file indexes in DWARF are one based and a file of index zero is
                // supposed to be the compile unit itself.
                support_files.Append (*sc.comp_unit);
                return DWARFDebugLine::ParseSupportFiles(sc.comp_unit->GetModule(),
                                                         get_debug_line_data(),
                                                         cu_comp_dir,
                                                         stmt_list,
                                                         support_files);
            }
        }
    }
    return false;
}

bool
SymbolFileDWARF::ParseImportedModules (const lldb_private::SymbolContext &sc, std::vector<lldb_private::ConstString> &imported_modules)
{
    assert (sc.comp_unit);
    DWARFCompileUnit* dwarf_cu = GetDWARFCompileUnit(sc.comp_unit);
    if (dwarf_cu)
    {
        if (ClangModulesDeclVendor::LanguageSupportsClangModules(sc.comp_unit->GetLanguage()))
        {
            UpdateExternalModuleListIfNeeded();
            
            if (sc.comp_unit)
            {
                const DWARFDIE die = dwarf_cu->GetCompileUnitDIEOnly();
                
                if (die)
                {
                    for (DWARFDIE child_die = die.GetFirstChild();
                         child_die;
                         child_die = child_die.GetSibling())
                    {
                        if (child_die.Tag() == DW_TAG_imported_declaration)
                        {
                            if (DWARFDIE module_die = child_die.GetReferencedDIE(DW_AT_import))
                            {
                                if (module_die.Tag() == DW_TAG_module)
                                {
                                    if (const char *name = module_die.GetAttributeValueAsString(DW_AT_name, nullptr))
                                    {
                                        ConstString const_name(name);
                                        imported_modules.push_back(const_name);
                                    }
                                }
                            }
                        }
                    }
                }
            }
            else
            {
                for (const auto &pair : m_external_type_modules)
                {
                    imported_modules.push_back(pair.first);
                }
            }
        }
    }
    return false;
}

struct ParseDWARFLineTableCallbackInfo
{
    LineTable* line_table;
    std::unique_ptr<LineSequence> sequence_ap;
    lldb::addr_t addr_mask;
};

//----------------------------------------------------------------------
// ParseStatementTableCallback
//----------------------------------------------------------------------
static void
ParseDWARFLineTableCallback(dw_offset_t offset, const DWARFDebugLine::State& state, void* userData)
{
    if (state.row == DWARFDebugLine::State::StartParsingLineTable)
    {
        // Just started parsing the line table
    }
    else if (state.row == DWARFDebugLine::State::DoneParsingLineTable)
    {
        // Done parsing line table, nothing to do for the cleanup
    }
    else
    {
        ParseDWARFLineTableCallbackInfo* info = (ParseDWARFLineTableCallbackInfo*)userData;
        LineTable* line_table = info->line_table;

        // If this is our first time here, we need to create a
        // sequence container.
        if (!info->sequence_ap.get())
        {
            info->sequence_ap.reset(line_table->CreateLineSequenceContainer());
            assert(info->sequence_ap.get());
        }
        line_table->AppendLineEntryToSequence (info->sequence_ap.get(),
                                               state.address & info->addr_mask,
                                               state.line,
                                               state.column,
                                               state.file,
                                               state.is_stmt,
                                               state.basic_block,
                                               state.prologue_end,
                                               state.epilogue_begin,
                                               state.end_sequence);
        if (state.end_sequence)
        {
            // First, put the current sequence into the line table.
            line_table->InsertSequence(info->sequence_ap.get());
            // Then, empty it to prepare for the next sequence.
            info->sequence_ap->Clear();
        }
    }
}

bool
SymbolFileDWARF::ParseCompileUnitLineTable (const SymbolContext &sc)
{
    assert (sc.comp_unit);
    if (sc.comp_unit->GetLineTable() != NULL)
        return true;

    DWARFCompileUnit* dwarf_cu = GetDWARFCompileUnit(sc.comp_unit);
    if (dwarf_cu)
    {
        const DWARFDIE dwarf_cu_die = dwarf_cu->GetCompileUnitDIEOnly();
        if (dwarf_cu_die)
        {
            const dw_offset_t cu_line_offset = dwarf_cu_die.GetAttributeValueAsUnsigned(DW_AT_stmt_list, DW_INVALID_OFFSET);
            if (cu_line_offset != DW_INVALID_OFFSET)
            {
                std::unique_ptr<LineTable> line_table_ap(new LineTable(sc.comp_unit));
                if (line_table_ap.get())
                {
                    ParseDWARFLineTableCallbackInfo info;
                    info.line_table = line_table_ap.get();

                    /*
                     * MIPS:
                     * The SymbolContext may not have a valid target, thus we may not be able
                     * to call Address::GetOpcodeLoadAddress() which would clear the bit #0
                     * for MIPS. Use ArchSpec to clear the bit #0.
                    */
                    ArchSpec arch;
                    GetObjectFile()->GetArchitecture(arch);
                    switch (arch.GetMachine())
                    {
                    case llvm::Triple::mips:
                    case llvm::Triple::mipsel:
                    case llvm::Triple::mips64:
                    case llvm::Triple::mips64el:
                        info.addr_mask = ~((lldb::addr_t)1);
                        break;
                    default:
                        info.addr_mask = ~((lldb::addr_t)0);
                        break;
                    }

                    lldb::offset_t offset = cu_line_offset;
                    DWARFDebugLine::ParseStatementTable(get_debug_line_data(), &offset, ParseDWARFLineTableCallback, &info);
                    if (m_debug_map_symfile)
                    {
                        // We have an object file that has a line table with addresses
                        // that are not linked. We need to link the line table and convert
                        // the addresses that are relative to the .o file into addresses
                        // for the main executable.
                        sc.comp_unit->SetLineTable (m_debug_map_symfile->LinkOSOLineTable (this, line_table_ap.get()));
                    }
                    else
                    {
                        sc.comp_unit->SetLineTable(line_table_ap.release());
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

lldb_private::DebugMacrosSP
SymbolFileDWARF::ParseDebugMacros(lldb::offset_t *offset)
{
    auto iter = m_debug_macros_map.find(*offset);
    if (iter != m_debug_macros_map.end())
        return iter->second;

    const DWARFDataExtractor &debug_macro_data = get_debug_macro_data();
    if (debug_macro_data.GetByteSize() == 0)
        return DebugMacrosSP();

    lldb_private::DebugMacrosSP debug_macros_sp(new lldb_private::DebugMacros());
    m_debug_macros_map[*offset] = debug_macros_sp;

    const DWARFDebugMacroHeader &header = DWARFDebugMacroHeader::ParseHeader(debug_macro_data, offset);
    DWARFDebugMacroEntry::ReadMacroEntries(
        debug_macro_data, get_debug_str_data(), header.OffsetIs64Bit(), offset, this, debug_macros_sp);

    return debug_macros_sp;
}

bool
SymbolFileDWARF::ParseCompileUnitDebugMacros(const SymbolContext& sc)
{
    assert (sc.comp_unit);

    DWARFCompileUnit* dwarf_cu = GetDWARFCompileUnit(sc.comp_unit);
    if (dwarf_cu == nullptr)
        return false;

    const DWARFDIE dwarf_cu_die = dwarf_cu->GetCompileUnitDIEOnly();
    if (!dwarf_cu_die)
        return false;

    lldb::offset_t sect_offset = dwarf_cu_die.GetAttributeValueAsUnsigned(DW_AT_macros, DW_INVALID_OFFSET);
    if (sect_offset == DW_INVALID_OFFSET)
        sect_offset = dwarf_cu_die.GetAttributeValueAsUnsigned(DW_AT_GNU_macros, DW_INVALID_OFFSET);
    if (sect_offset == DW_INVALID_OFFSET)
        return false;

    sc.comp_unit->SetDebugMacros(ParseDebugMacros(&sect_offset));

    return true;
}

size_t
SymbolFileDWARF::ParseFunctionBlocks (const SymbolContext& sc,
                                      Block *parent_block,
                                      const DWARFDIE &orig_die,
                                      addr_t subprogram_low_pc,
                                      uint32_t depth)
{
    size_t blocks_added = 0;
    DWARFDIE die = orig_die;
    while (die)
    {
        dw_tag_t tag = die.Tag();

        switch (tag)
        {
        case DW_TAG_inlined_subroutine:
        case DW_TAG_subprogram:
        case DW_TAG_lexical_block:
            {
                Block *block = NULL;
                if (tag == DW_TAG_subprogram)
                {
                    // Skip any DW_TAG_subprogram DIEs that are inside
                    // of a normal or inlined functions. These will be 
                    // parsed on their own as separate entities.

                    if (depth > 0)
                        break;

                    block = parent_block;
                }
                else
                {
                    BlockSP block_sp(new Block (die.GetID()));
                    parent_block->AddChild(block_sp);
                    block = block_sp.get();
                }
                DWARFRangeList ranges;
                const char *name = NULL;
                const char *mangled_name = NULL;

                int decl_file = 0;
                int decl_line = 0;
                int decl_column = 0;
                int call_file = 0;
                int call_line = 0;
                int call_column = 0;
                if (die.GetDIENamesAndRanges (name,
                                              mangled_name,
                                              ranges,
                                              decl_file, decl_line, decl_column,
                                              call_file, call_line, call_column, nullptr))
                {
                    if (tag == DW_TAG_subprogram)
                    {
                        assert (subprogram_low_pc == LLDB_INVALID_ADDRESS);
                        subprogram_low_pc = ranges.GetMinRangeBase(0);
                    }
                    else if (tag == DW_TAG_inlined_subroutine)
                    {
                        // We get called here for inlined subroutines in two ways.  
                        // The first time is when we are making the Function object 
                        // for this inlined concrete instance.  Since we're creating a top level block at
                        // here, the subprogram_low_pc will be LLDB_INVALID_ADDRESS.  So we need to 
                        // adjust the containing address.
                        // The second time is when we are parsing the blocks inside the function that contains
                        // the inlined concrete instance.  Since these will be blocks inside the containing "real"
                        // function the offset will be for that function.  
                        if (subprogram_low_pc == LLDB_INVALID_ADDRESS)
                        {
                            subprogram_low_pc = ranges.GetMinRangeBase(0);
                        }
                    }

                    const size_t num_ranges = ranges.GetSize();
                    for (size_t i = 0; i<num_ranges; ++i)
                    {
                        const DWARFRangeList::Entry &range = ranges.GetEntryRef (i);
                        const addr_t range_base = range.GetRangeBase();
                        if (range_base >= subprogram_low_pc)
                            block->AddRange(Block::Range (range_base - subprogram_low_pc, range.GetByteSize()));
                        else
                        {
                            GetObjectFile()->GetModule()->ReportError ("0x%8.8" PRIx64 ": adding range [0x%" PRIx64 "-0x%" PRIx64 ") which has a base that is less than the function's low PC 0x%" PRIx64 ". Please file a bug and attach the file at the start of this error message",
                                                                       block->GetID(),
                                                                       range_base,
                                                                       range.GetRangeEnd(),
                                                                       subprogram_low_pc);
                        }
                    }
                    block->FinalizeRanges ();

                    if (tag != DW_TAG_subprogram && (name != NULL || mangled_name != NULL))
                    {
                        std::unique_ptr<Declaration> decl_ap;
                        if (decl_file != 0 || decl_line != 0 || decl_column != 0)
                            decl_ap.reset(new Declaration(sc.comp_unit->GetSupportFiles().GetFileSpecAtIndex(decl_file), 
                                                          decl_line, decl_column));

                        std::unique_ptr<Declaration> call_ap;
                        if (call_file != 0 || call_line != 0 || call_column != 0)
                            call_ap.reset(new Declaration(sc.comp_unit->GetSupportFiles().GetFileSpecAtIndex(call_file), 
                                                          call_line, call_column));

                        block->SetInlinedFunctionInfo (name, mangled_name, decl_ap.get(), call_ap.get());
                    }

                    ++blocks_added;

                    if (die.HasChildren())
                    {
                        blocks_added += ParseFunctionBlocks (sc, 
                                                             block, 
                                                             die.GetFirstChild(),
                                                             subprogram_low_pc, 
                                                             depth + 1);
                    }
                }
            }
            break;
        default:
            break;
        }

        // Only parse siblings of the block if we are not at depth zero. A depth
        // of zero indicates we are currently parsing the top level 
        // DW_TAG_subprogram DIE
        
        if (depth == 0)
            die.Clear();
        else
            die = die.GetSibling();
    }
    return blocks_added;
}

bool
SymbolFileDWARF::ClassOrStructIsVirtual (const DWARFDIE &parent_die)
{
    if (parent_die)
    {
        for (DWARFDIE die = parent_die.GetFirstChild(); die; die = die.GetSibling())
        {
            dw_tag_t tag = die.Tag();
            bool check_virtuality = false;
            switch (tag)
            {
                case DW_TAG_inheritance:
                case DW_TAG_subprogram:
                    check_virtuality = true;
                    break;
                default:
                    break;
            }
            if (check_virtuality)
            {
                if (die.GetAttributeValueAsUnsigned(DW_AT_virtuality, 0) != 0)
                    return true;
            }
        }
    }
    return false;
}

void
SymbolFileDWARF::ParseDeclsForContext (CompilerDeclContext decl_ctx)
{
    TypeSystem *type_system = decl_ctx.GetTypeSystem();
    DWARFASTParser *ast_parser = type_system->GetDWARFParser();
    std::vector<DWARFDIE> decl_ctx_die_list = ast_parser->GetDIEForDeclContext(decl_ctx);

    for (DWARFDIE decl_ctx_die : decl_ctx_die_list)
        for (DWARFDIE decl = decl_ctx_die.GetFirstChild(); decl; decl = decl.GetSibling())
            ast_parser->GetDeclForUIDFromDWARF(decl);
}

CompilerDecl
SymbolFileDWARF::GetDeclForUID (lldb::user_id_t type_uid)
{
    if (UserIDMatches(type_uid))
    {
        DWARFDebugInfo* debug_info = DebugInfo();
        if (debug_info)
        {
            DWARFDIE die = debug_info->GetDIE(DIERef(type_uid));
            if (die)
            {
                DWARFASTParser *dwarf_ast = die.GetDWARFParser();
                if (dwarf_ast)
                    return dwarf_ast->GetDeclForUIDFromDWARF(die);
            }
        }
    }
    return CompilerDecl();
}

CompilerDeclContext
SymbolFileDWARF::GetDeclContextForUID (lldb::user_id_t type_uid)
{
    if (UserIDMatches(type_uid))
    {
        DWARFDebugInfo* debug_info = DebugInfo();
        if (debug_info)
        {
            DWARFDIE die = debug_info->GetDIE(DIERef(type_uid));
            if (die)
            {
                DWARFASTParser *dwarf_ast = die.GetDWARFParser();
                if (dwarf_ast)
                    return dwarf_ast->GetDeclContextForUIDFromDWARF(die);
            }
        }
    }
    return CompilerDeclContext();
}

CompilerDeclContext
SymbolFileDWARF::GetDeclContextContainingUID (lldb::user_id_t type_uid)
{
    if (UserIDMatches(type_uid))
    {
        DWARFDebugInfo* debug_info = DebugInfo();
        if (debug_info)
        {
            DWARFDIE die = debug_info->GetDIE(DIERef(type_uid));
            if (die)
            {
                DWARFASTParser *dwarf_ast = die.GetDWARFParser();
                if (dwarf_ast)
                    return dwarf_ast->GetDeclContextContainingUIDFromDWARF(die);
            }
        }
    }
    return CompilerDeclContext();
}


Type*
SymbolFileDWARF::ResolveTypeUID (lldb::user_id_t type_uid)
{
    if (UserIDMatches(type_uid))
    {
        DWARFDebugInfo* debug_info = DebugInfo();
        if (debug_info)
        {
            DWARFDIE type_die = debug_info->GetDIE (DIERef(type_uid));
            if (type_die)
            {
                const bool assert_not_being_parsed = true;
                return ResolveTypeUID (type_die, assert_not_being_parsed);
            }
        }
    }
    return NULL;
}

Type*
SymbolFileDWARF::ResolveTypeUID (const DWARFDIE &die, bool assert_not_being_parsed)
{    
    if (die)
    {
        Log *log (LogChannelDWARF::GetLogIfAll(DWARF_LOG_DEBUG_INFO));
        if (log)
            GetObjectFile()->GetModule()->LogMessage (log,
                                                      "SymbolFileDWARF::ResolveTypeUID (die = 0x%8.8x) %s '%s'", 
                                                      die.GetOffset(),
                                                      die.GetTagAsCString(),
                                                      die.GetName());

        // We might be coming in in the middle of a type tree (a class
        // withing a class, an enum within a class), so parse any needed
        // parent DIEs before we get to this one...
        DWARFDIE decl_ctx_die = GetDeclContextDIEContainingDIE (die);
        if (decl_ctx_die)
        {
            if (log)
            {
                switch (decl_ctx_die.Tag())
                {
                    case DW_TAG_structure_type:
                    case DW_TAG_union_type:
                    case DW_TAG_class_type:
                    {
                        // Get the type, which could be a forward declaration
                        if (log)
                            GetObjectFile()->GetModule()->LogMessage (log,
                                                                      "SymbolFileDWARF::ResolveTypeUID (die = 0x%8.8x) %s '%s' resolve parent forward type for 0x%8.8x", 
                                                                      die.GetOffset(),
                                                                      die.GetTagAsCString(),
                                                                      die.GetName(),
                                                                      decl_ctx_die.GetOffset());
                    }
                    break;

                    default:
                        break;
                }
            }
        }
        return ResolveType (die);
    }
    return NULL;
}

// This function is used when SymbolFileDWARFDebugMap owns a bunch of
// SymbolFileDWARF objects to detect if this DWARF file is the one that
// can resolve a compiler_type.
bool
SymbolFileDWARF::HasForwardDeclForClangType (const CompilerType &compiler_type)
{
    CompilerType compiler_type_no_qualifiers = ClangASTContext::RemoveFastQualifiers(compiler_type);
    if (GetForwardDeclClangTypeToDie().count (compiler_type_no_qualifiers.GetOpaqueQualType()))
    {
        return true;
    }
    TypeSystem *type_system = compiler_type.GetTypeSystem();
    if (type_system)
    {
        DWARFASTParser *dwarf_ast = type_system->GetDWARFParser();
        if (dwarf_ast)
            return dwarf_ast->CanCompleteType(compiler_type);
    }
    return false;
}


bool
SymbolFileDWARF::CompleteType (CompilerType &compiler_type)
{
    lldb_private::Mutex::Locker locker(GetObjectFile()->GetModule()->GetMutex());

    TypeSystem *type_system = compiler_type.GetTypeSystem();
    if (type_system)
    {
        DWARFASTParser *dwarf_ast = type_system->GetDWARFParser();
        if (dwarf_ast && dwarf_ast->CanCompleteType(compiler_type))
            return dwarf_ast->CompleteType(compiler_type);
    }

    // We have a struct/union/class/enum that needs to be fully resolved.
    CompilerType compiler_type_no_qualifiers = ClangASTContext::RemoveFastQualifiers(compiler_type);
    auto die_it = GetForwardDeclClangTypeToDie().find (compiler_type_no_qualifiers.GetOpaqueQualType());
    if (die_it == GetForwardDeclClangTypeToDie().end())
    {
        // We have already resolved this type...
        return true;
    }

    DWARFDebugInfo* debug_info = DebugInfo();
    DWARFDIE dwarf_die = debug_info->GetDIE(die_it->getSecond());

    assert(UserIDMatches(die_it->getSecond().GetUID()) && "CompleteType called on the wrong SymbolFile");

    // Once we start resolving this type, remove it from the forward declaration
    // map in case anyone child members or other types require this type to get resolved.
    // The type will get resolved when all of the calls to SymbolFileDWARF::ResolveClangOpaqueTypeDefinition
    // are done.
    GetForwardDeclClangTypeToDie().erase (die_it);

    Type *type = GetDIEToType().lookup (dwarf_die.GetDIE());

    Log *log (LogChannelDWARF::GetLogIfAny(DWARF_LOG_DEBUG_INFO|DWARF_LOG_TYPE_COMPLETION));
    if (log)
        GetObjectFile()->GetModule()->LogMessageVerboseBacktrace (log,
                                                                  "0x%8.8" PRIx64 ": %s '%s' resolving forward declaration...",
                                                                  dwarf_die.GetID(),
                                                                  dwarf_die.GetTagAsCString(),
                                                                  type->GetName().AsCString());
    assert (compiler_type);
    DWARFASTParser *dwarf_ast = dwarf_die.GetDWARFParser();
    if (dwarf_ast)
        return dwarf_ast->CompleteTypeFromDWARF (dwarf_die, type, compiler_type);
    return false;
}

Type*
SymbolFileDWARF::ResolveType (const DWARFDIE &die, bool assert_not_being_parsed, bool resolve_function_context)
{
    if (die)
    {
        Type *type = GetDIEToType().lookup (die.GetDIE());

        if (type == NULL)
            type = GetTypeForDIE (die, resolve_function_context).get();

        if (assert_not_being_parsed)
        { 
            if (type != DIE_IS_BEING_PARSED)
                return type;
            
            GetObjectFile()->GetModule()->ReportError ("Parsing a die that is being parsed die: 0x%8.8x: %s %s",
                                                       die.GetOffset(),
                                                       die.GetTagAsCString(),
                                                       die.GetName());

        }
        else
            return type;
    }
    return nullptr;
}

CompileUnit*
SymbolFileDWARF::GetCompUnitForDWARFCompUnit (DWARFCompileUnit* dwarf_cu, uint32_t cu_idx)
{
    // Check if the symbol vendor already knows about this compile unit?
    if (dwarf_cu->GetUserData() == NULL)
    {
        // The symbol vendor doesn't know about this compile unit, we
        // need to parse and add it to the symbol vendor object.
        return ParseCompileUnit(dwarf_cu, cu_idx).get();
    }
    return (CompileUnit*)dwarf_cu->GetUserData();
}

size_t
SymbolFileDWARF::GetObjCMethodDIEOffsets (ConstString class_name, DIEArray &method_die_offsets)
{
    method_die_offsets.clear();
    if (m_using_apple_tables)
    {
        if (m_apple_objc_ap.get())
            m_apple_objc_ap->FindByName(class_name.GetCString(), method_die_offsets);
    }
    else
    {
        if (!m_indexed)
            Index ();

        m_objc_class_selectors_index.Find (class_name, method_die_offsets);
    }
    return method_die_offsets.size();
}

bool
SymbolFileDWARF::GetFunction (const DWARFDIE &die, SymbolContext& sc)
{
    sc.Clear(false);

    if (die)
    {
        // Check if the symbol vendor already knows about this compile unit?
        sc.comp_unit = GetCompUnitForDWARFCompUnit(die.GetCU(), UINT32_MAX);

        sc.function = sc.comp_unit->FindFunctionByUID (die.GetID()).get();
        if (sc.function == NULL)
            sc.function = ParseCompileUnitFunction(sc, die);
            
        if (sc.function)
        {        
            sc.module_sp = sc.function->CalculateSymbolContextModule();
            return true;
        }
    }
    
    return false;
}

lldb::ModuleSP
SymbolFileDWARF::GetDWOModule (ConstString name)
{
    UpdateExternalModuleListIfNeeded();
    const auto &pos = m_external_type_modules.find(name);
    if (pos != m_external_type_modules.end())
        return pos->second;
    else
        return lldb::ModuleSP();
}

void
SymbolFileDWARF::UpdateExternalModuleListIfNeeded()
{
    if (m_fetched_external_modules)
        return;
    m_fetched_external_modules = true;
    
    DWARFDebugInfo * debug_info = DebugInfo();

    const uint32_t num_compile_units = GetNumCompileUnits();
    for (uint32_t cu_idx = 0; cu_idx < num_compile_units; ++cu_idx)
    {
        DWARFCompileUnit* dwarf_cu = debug_info->GetCompileUnitAtIndex(cu_idx);
        
        const DWARFDIE die = dwarf_cu->GetCompileUnitDIEOnly();
        if (die && die.HasChildren() == false)
        {
            const char *name = die.GetAttributeValueAsString(DW_AT_name, nullptr);

            if (name)
            {
                ConstString const_name(name);
                if (m_external_type_modules.find(const_name) == m_external_type_modules.end())
                {
                    ModuleSP module_sp;
                    const char *dwo_path = die.GetAttributeValueAsString(DW_AT_GNU_dwo_name, nullptr);
                    if (dwo_path)
                    {
                        ModuleSpec dwo_module_spec;
                        dwo_module_spec.GetFileSpec().SetFile(dwo_path, false);
                        dwo_module_spec.GetArchitecture() = m_obj_file->GetModule()->GetArchitecture();
                        //printf ("Loading dwo = '%s'\n", dwo_path);
                        Error error = ModuleList::GetSharedModule (dwo_module_spec, module_sp, NULL, NULL, NULL);
                    }
                    m_external_type_modules[const_name] = module_sp;
                }
            }
        }
    }
}

SymbolFileDWARF::GlobalVariableMap &
SymbolFileDWARF::GetGlobalAranges()
{
    if (!m_global_aranges_ap)
    {
        m_global_aranges_ap.reset (new GlobalVariableMap());

        ModuleSP module_sp = GetObjectFile()->GetModule();
        if (module_sp)
        {
            const size_t num_cus = module_sp->GetNumCompileUnits();
            for (size_t i = 0; i < num_cus; ++i)
            {
                CompUnitSP cu_sp = module_sp->GetCompileUnitAtIndex(i);
                if (cu_sp)
                {
                    VariableListSP globals_sp = cu_sp->GetVariableList(true);
                    if (globals_sp)
                    {
                        const size_t num_globals = globals_sp->GetSize();
                        for (size_t g = 0; g < num_globals; ++g)
                        {
                            VariableSP var_sp = globals_sp->GetVariableAtIndex(g);
                            if (var_sp && !var_sp->GetLocationIsConstantValueData())
                            {
                                const DWARFExpression &location = var_sp->LocationExpression();
                                Value location_result;
                                Error error;
                                if (location.Evaluate(nullptr, nullptr, nullptr, LLDB_INVALID_ADDRESS, nullptr, nullptr, location_result, &error))
                                {
                                    if (location_result.GetValueType() == Value::eValueTypeFileAddress)
                                    {
                                        lldb::addr_t file_addr = location_result.GetScalar().ULongLong();
                                        lldb::addr_t byte_size = 1;
                                        if (var_sp->GetType())
                                            byte_size = var_sp->GetType()->GetByteSize();
                                        m_global_aranges_ap->Append(GlobalVariableMap::Entry(file_addr, byte_size, var_sp.get()));
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        m_global_aranges_ap->Sort();
    }
    return *m_global_aranges_ap;
}


uint32_t
SymbolFileDWARF::ResolveSymbolContext (const Address& so_addr, uint32_t resolve_scope, SymbolContext& sc)
{
    Timer scoped_timer(__PRETTY_FUNCTION__,
                       "SymbolFileDWARF::ResolveSymbolContext (so_addr = { section = %p, offset = 0x%" PRIx64 " }, resolve_scope = 0x%8.8x)",
                       static_cast<void*>(so_addr.GetSection().get()),
                       so_addr.GetOffset(), resolve_scope);
    uint32_t resolved = 0;
    if (resolve_scope & (   eSymbolContextCompUnit  |
                            eSymbolContextFunction  |
                            eSymbolContextBlock     |
                            eSymbolContextLineEntry |
                            eSymbolContextVariable  ))
    {
        lldb::addr_t file_vm_addr = so_addr.GetFileAddress();

        DWARFDebugInfo* debug_info = DebugInfo();
        if (debug_info)
        {
            const dw_offset_t cu_offset = debug_info->GetCompileUnitAranges().FindAddress(file_vm_addr);
            if (cu_offset == DW_INVALID_OFFSET)
            {
                // Global variables are not in the compile unit address ranges. The only way to
                // currently find global variables is to iterate over the .debug_pubnames or the
                // __apple_names table and find all items in there that point to DW_TAG_variable
                // DIEs and then find the address that matches.
                if (resolve_scope & eSymbolContextVariable)
                {
                    GlobalVariableMap &map = GetGlobalAranges();
                    const GlobalVariableMap::Entry *entry = map.FindEntryThatContains(file_vm_addr);
                    if (entry && entry->data)
                    {
                        Variable *variable = entry->data;
                        SymbolContextScope *scc = variable->GetSymbolContextScope();
                        if (scc)
                        {
                            scc->CalculateSymbolContext(&sc);
                            sc.variable = variable;
                        }
                        return sc.GetResolvedMask();
                    }
                }
            }
            else
            {
                uint32_t cu_idx = DW_INVALID_INDEX;
                DWARFCompileUnit* dwarf_cu = debug_info->GetCompileUnit(cu_offset, &cu_idx);
                if (dwarf_cu)
                {
                    sc.comp_unit = GetCompUnitForDWARFCompUnit(dwarf_cu, cu_idx);
                    if (sc.comp_unit)
                    {
                        resolved |= eSymbolContextCompUnit;

                        bool force_check_line_table = false;
                        if (resolve_scope & (eSymbolContextFunction | eSymbolContextBlock))
                        {
                            DWARFDIE function_die = dwarf_cu->LookupAddress(file_vm_addr);
                            DWARFDIE block_die;
                            if (function_die)
                            {
                                sc.function = sc.comp_unit->FindFunctionByUID (function_die.GetID()).get();
                                if (sc.function == NULL)
                                    sc.function = ParseCompileUnitFunction(sc, function_die);

                                if (sc.function && (resolve_scope & eSymbolContextBlock))
                                    block_die = function_die.LookupDeepestBlock(file_vm_addr);
                            }
                            else
                            {
                                // We might have had a compile unit that had discontiguous
                                // address ranges where the gaps are symbols that don't have
                                // any debug info. Discontiguous compile unit address ranges
                                // should only happen when there aren't other functions from
                                // other compile units in these gaps. This helps keep the size
                                // of the aranges down.
                                force_check_line_table = true;
                            }

                            if (sc.function != NULL)
                            {
                                resolved |= eSymbolContextFunction;

                                if (resolve_scope & eSymbolContextBlock)
                                {
                                    Block& block = sc.function->GetBlock (true);

                                    if (block_die)
                                        sc.block = block.FindBlockByID (block_die.GetID());
                                    else
                                        sc.block = block.FindBlockByID (function_die.GetID());
                                    if (sc.block)
                                        resolved |= eSymbolContextBlock;
                                }
                            }
                        }
                        
                        if ((resolve_scope & eSymbolContextLineEntry) || force_check_line_table)
                        {
                            LineTable *line_table = sc.comp_unit->GetLineTable();
                            if (line_table != NULL)
                            {
                                // And address that makes it into this function should be in terms
                                // of this debug file if there is no debug map, or it will be an
                                // address in the .o file which needs to be fixed up to be in terms
                                // of the debug map executable. Either way, calling FixupAddress()
                                // will work for us.
                                Address exe_so_addr (so_addr);
                                if (FixupAddress(exe_so_addr))
                                {
                                    if (line_table->FindLineEntryByAddress (exe_so_addr, sc.line_entry))
                                    {
                                        resolved |= eSymbolContextLineEntry;
                                    }
                                }
                            }
                        }
                        
                        if (force_check_line_table && !(resolved & eSymbolContextLineEntry))
                        {
                            // We might have had a compile unit that had discontiguous
                            // address ranges where the gaps are symbols that don't have
                            // any debug info. Discontiguous compile unit address ranges
                            // should only happen when there aren't other functions from
                            // other compile units in these gaps. This helps keep the size
                            // of the aranges down.
                            sc.comp_unit = NULL;
                            resolved &= ~eSymbolContextCompUnit;
                        }
                    }
                    else
                    {
                        GetObjectFile()->GetModule()->ReportWarning ("0x%8.8x: compile unit %u failed to create a valid lldb_private::CompileUnit class.",
                                                                     cu_offset,
                                                                     cu_idx);
                    }
                }
            }
        }
    }
    return resolved;
}



uint32_t
SymbolFileDWARF::ResolveSymbolContext(const FileSpec& file_spec, uint32_t line, bool check_inlines, uint32_t resolve_scope, SymbolContextList& sc_list)
{
    const uint32_t prev_size = sc_list.GetSize();
    if (resolve_scope & eSymbolContextCompUnit)
    {
        DWARFDebugInfo* debug_info = DebugInfo();
        if (debug_info)
        {
            uint32_t cu_idx;
            DWARFCompileUnit* dwarf_cu = NULL;

            for (cu_idx = 0; (dwarf_cu = debug_info->GetCompileUnitAtIndex(cu_idx)) != NULL; ++cu_idx)
            {
                CompileUnit *dc_cu = GetCompUnitForDWARFCompUnit(dwarf_cu, cu_idx);
                const bool full_match = (bool)file_spec.GetDirectory();
                bool file_spec_matches_cu_file_spec = dc_cu != NULL && FileSpec::Equal(file_spec, *dc_cu, full_match);
                if (check_inlines || file_spec_matches_cu_file_spec)
                {
                    SymbolContext sc (m_obj_file->GetModule());
                    sc.comp_unit = GetCompUnitForDWARFCompUnit(dwarf_cu, cu_idx);
                    if (sc.comp_unit)
                    {
                        uint32_t file_idx = UINT32_MAX;

                        // If we are looking for inline functions only and we don't
                        // find it in the support files, we are done.
                        if (check_inlines)
                        {
                            file_idx = sc.comp_unit->GetSupportFiles().FindFileIndex (1, file_spec, true);
                            if (file_idx == UINT32_MAX)
                                continue;
                        }

                        if (line != 0)
                        {
                            LineTable *line_table = sc.comp_unit->GetLineTable();

                            if (line_table != NULL && line != 0)
                            {
                                // We will have already looked up the file index if
                                // we are searching for inline entries.
                                if (!check_inlines)
                                    file_idx = sc.comp_unit->GetSupportFiles().FindFileIndex (1, file_spec, true);

                                if (file_idx != UINT32_MAX)
                                {
                                    uint32_t found_line;
                                    uint32_t line_idx = line_table->FindLineEntryIndexByFileIndex (0, file_idx, line, false, &sc.line_entry);
                                    found_line = sc.line_entry.line;

                                    while (line_idx != UINT32_MAX)
                                    {
                                        sc.function = NULL;
                                        sc.block = NULL;
                                        if (resolve_scope & (eSymbolContextFunction | eSymbolContextBlock))
                                        {
                                            const lldb::addr_t file_vm_addr = sc.line_entry.range.GetBaseAddress().GetFileAddress();
                                            if (file_vm_addr != LLDB_INVALID_ADDRESS)
                                            {
                                                DWARFDIE function_die = dwarf_cu->LookupAddress(file_vm_addr);
                                                DWARFDIE block_die;
                                                if (function_die)
                                                {
                                                    sc.function = sc.comp_unit->FindFunctionByUID (function_die.GetID()).get();
                                                    if (sc.function == NULL)
                                                        sc.function = ParseCompileUnitFunction(sc, function_die);

                                                    if (sc.function && (resolve_scope & eSymbolContextBlock))
                                                        block_die = function_die.LookupDeepestBlock(file_vm_addr);
                                                }

                                                if (sc.function != NULL)
                                                {
                                                    Block& block = sc.function->GetBlock (true);

                                                    if (block_die)
                                                        sc.block = block.FindBlockByID (block_die.GetID());
                                                    else if (function_die)
                                                        sc.block = block.FindBlockByID (function_die.GetID());
                                                }
                                            }
                                        }

                                        sc_list.Append(sc);
                                        line_idx = line_table->FindLineEntryIndexByFileIndex (line_idx + 1, file_idx, found_line, true, &sc.line_entry);
                                    }
                                }
                            }
                            else if (file_spec_matches_cu_file_spec && !check_inlines)
                            {
                                // only append the context if we aren't looking for inline call sites
                                // by file and line and if the file spec matches that of the compile unit
                                sc_list.Append(sc);
                            }
                        }
                        else if (file_spec_matches_cu_file_spec && !check_inlines)
                        {
                            // only append the context if we aren't looking for inline call sites
                            // by file and line and if the file spec matches that of the compile unit
                            sc_list.Append(sc);
                        }

                        if (!check_inlines)
                            break;
                    }
                }
            }
        }
    }
    return sc_list.GetSize() - prev_size;
}

void
SymbolFileDWARF::Index ()
{
    if (m_indexed)
        return;
    m_indexed = true;
    Timer scoped_timer (__PRETTY_FUNCTION__,
                        "SymbolFileDWARF::Index (%s)",
                        GetObjectFile()->GetFileSpec().GetFilename().AsCString("<Unknown>"));

    DWARFDebugInfo* debug_info = DebugInfo();
    if (debug_info)
    {
        const uint32_t num_compile_units = GetNumCompileUnits();
        std::vector<NameToDIE> function_basename_index(num_compile_units);
        std::vector<NameToDIE> function_fullname_index(num_compile_units);
        std::vector<NameToDIE> function_method_index(num_compile_units);
        std::vector<NameToDIE> function_selector_index(num_compile_units);
        std::vector<NameToDIE> objc_class_selectors_index(num_compile_units);
        std::vector<NameToDIE> global_index(num_compile_units);
        std::vector<NameToDIE> type_index(num_compile_units);
        std::vector<NameToDIE> namespace_index(num_compile_units);
        
        auto parser_fn = [this,
                          debug_info,
                          &function_basename_index,
                          &function_fullname_index,
                          &function_method_index,
                          &function_selector_index,
                          &objc_class_selectors_index,
                          &global_index,
                          &type_index,
                          &namespace_index](uint32_t cu_idx)
        {
            DWARFCompileUnit* dwarf_cu = debug_info->GetCompileUnitAtIndex(cu_idx);
            bool clear_dies = dwarf_cu->ExtractDIEsIfNeeded(false) > 1;

            dwarf_cu->Index(function_basename_index[cu_idx],
                            function_fullname_index[cu_idx],
                            function_method_index[cu_idx],
                            function_selector_index[cu_idx],
                            objc_class_selectors_index[cu_idx],
                            global_index[cu_idx],
                            type_index[cu_idx],
                            namespace_index[cu_idx]);

            // Keep memory down by clearing DIEs if this generate function
            // caused them to be parsed
            if (clear_dies)
                dwarf_cu->ClearDIEs(true);

            return cu_idx;
        };

        TaskRunner<uint32_t> task_runner;
        for (uint32_t cu_idx = 0; cu_idx < num_compile_units; ++cu_idx)
            task_runner.AddTask(parser_fn, cu_idx);

        while (true)
        {
            std::future<uint32_t> f = task_runner.WaitForNextCompletedTask();
            if (!f.valid())
                break;
            uint32_t cu_idx = f.get();

            m_function_basename_index.Append(function_basename_index[cu_idx]);
            m_function_fullname_index.Append(function_fullname_index[cu_idx]);
            m_function_method_index.Append(function_method_index[cu_idx]);
            m_function_selector_index.Append(function_selector_index[cu_idx]);
            m_objc_class_selectors_index.Append(objc_class_selectors_index[cu_idx]);
            m_global_index.Append(global_index[cu_idx]);
            m_type_index.Append(type_index[cu_idx]);
            m_namespace_index.Append(namespace_index[cu_idx]);
        }

        TaskPool::RunTasks(
            [&]() { m_function_basename_index.Finalize(); },
            [&]() { m_function_fullname_index.Finalize(); },
            [&]() { m_function_method_index.Finalize(); },
            [&]() { m_function_selector_index.Finalize(); },
            [&]() { m_objc_class_selectors_index.Finalize(); },
            [&]() { m_global_index.Finalize(); },
            [&]() { m_type_index.Finalize(); },
            [&]() { m_namespace_index.Finalize(); });

#if defined (ENABLE_DEBUG_PRINTF)
        StreamFile s(stdout, false);
        s.Printf ("DWARF index for '%s':",
                  GetObjectFile()->GetFileSpec().GetPath().c_str());
        s.Printf("\nFunction basenames:\n");    m_function_basename_index.Dump (&s);
        s.Printf("\nFunction fullnames:\n");    m_function_fullname_index.Dump (&s);
        s.Printf("\nFunction methods:\n");      m_function_method_index.Dump (&s);
        s.Printf("\nFunction selectors:\n");    m_function_selector_index.Dump (&s);
        s.Printf("\nObjective C class selectors:\n");    m_objc_class_selectors_index.Dump (&s);
        s.Printf("\nGlobals and statics:\n");   m_global_index.Dump (&s); 
        s.Printf("\nTypes:\n");                 m_type_index.Dump (&s);
        s.Printf("\nNamespaces:\n")             m_namespace_index.Dump (&s);
#endif
    }
}

bool
SymbolFileDWARF::DeclContextMatchesThisSymbolFile (const lldb_private::CompilerDeclContext *decl_ctx)
{
    if (decl_ctx == nullptr || !decl_ctx->IsValid())
    {
        // Invalid namespace decl which means we aren't matching only things
        // in this symbol file, so return true to indicate it matches this
        // symbol file.
        return true;
    }

    TypeSystem *decl_ctx_type_system = decl_ctx->GetTypeSystem();
    TypeSystem *type_system = GetTypeSystemForLanguage(decl_ctx_type_system->GetMinimumLanguage(nullptr));
    if (decl_ctx_type_system == type_system)
        return true;    // The type systems match, return true
    
    // The namespace AST was valid, and it does not match...
    Log *log (LogChannelDWARF::GetLogIfAll(DWARF_LOG_LOOKUPS));

    if (log)
        GetObjectFile()->GetModule()->LogMessage(log, "Valid namespace does not match symbol file");
    
    return false;
}

uint32_t
SymbolFileDWARF::FindGlobalVariables (const ConstString &name, const CompilerDeclContext *parent_decl_ctx, bool append, uint32_t max_matches, VariableList& variables)
{
    Log *log (LogChannelDWARF::GetLogIfAll(DWARF_LOG_LOOKUPS));

    if (log)
        GetObjectFile()->GetModule()->LogMessage (log,
                                                  "SymbolFileDWARF::FindGlobalVariables (name=\"%s\", parent_decl_ctx=%p, append=%u, max_matches=%u, variables)",
                                                  name.GetCString(), 
                                                  static_cast<const void*>(parent_decl_ctx),
                                                  append, max_matches);

    if (!DeclContextMatchesThisSymbolFile(parent_decl_ctx))
        return 0;

    DWARFDebugInfo* info = DebugInfo();
    if (info == NULL)
        return 0;

    // If we aren't appending the results to this list, then clear the list
    if (!append)
        variables.Clear();

    // Remember how many variables are in the list before we search in case
    // we are appending the results to a variable list.
    const uint32_t original_size = variables.GetSize();

    DIEArray die_offsets;

    if (m_using_apple_tables)
    {
        if (m_apple_names_ap.get())
        {
            const char *name_cstr = name.GetCString();
            llvm::StringRef basename;
            llvm::StringRef context;

            if (!CPlusPlusLanguage::ExtractContextAndIdentifier(name_cstr, context, basename))
                basename = name_cstr;

            m_apple_names_ap->FindByName (basename.data(), die_offsets);
        }
    }
    else
    {
        // Index the DWARF if we haven't already
        if (!m_indexed)
            Index ();

        m_global_index.Find (name, die_offsets);
    }

    const size_t num_die_matches = die_offsets.size();
    if (num_die_matches)
    {
        SymbolContext sc;
        sc.module_sp = m_obj_file->GetModule();
        assert (sc.module_sp);

        DWARFDebugInfo* debug_info = DebugInfo();
        bool done = false;
        for (size_t i=0; i<num_die_matches && !done; ++i)
        {
            const DIERef& die_ref = die_offsets[i];
            DWARFDIE die = debug_info->GetDIE (die_ref);

            if (die)
            {
                switch (die.Tag())
                {
                    default:
                    case DW_TAG_subprogram:
                    case DW_TAG_inlined_subroutine:
                    case DW_TAG_try_block:
                    case DW_TAG_catch_block:
                        break;

                    case DW_TAG_variable:
                        {
                            sc.comp_unit = GetCompUnitForDWARFCompUnit(die.GetCU(), UINT32_MAX);

                            if (parent_decl_ctx)
                            {
                                DWARFASTParser *dwarf_ast = die.GetDWARFParser();
                                if (dwarf_ast)
                                {
                                    CompilerDeclContext actual_parent_decl_ctx = dwarf_ast->GetDeclContextContainingUIDFromDWARF (die);
                                    if (!actual_parent_decl_ctx || actual_parent_decl_ctx != *parent_decl_ctx)
                                        continue;
                                }
                            }

                            ParseVariables(sc, die, LLDB_INVALID_ADDRESS, false, false, &variables);

                            if (variables.GetSize() - original_size >= max_matches)
                                done = true;
                        }
                        break;
                }
            }
            else
            {
                if (m_using_apple_tables)
                {
                    GetObjectFile()->GetModule()->ReportErrorIfModifyDetected ("the DWARF debug information has been modified (.apple_names accelerator table had bad die 0x%8.8x for '%s')\n",
                                                                               die_ref.die_offset, name.GetCString());
                }
            }
        }
    }

    // Return the number of variable that were appended to the list
    const uint32_t num_matches = variables.GetSize() - original_size;
    if (log && num_matches > 0)
    {
        GetObjectFile()->GetModule()->LogMessage (log,
                                                  "SymbolFileDWARF::FindGlobalVariables (name=\"%s\", parent_decl_ctx=%p, append=%u, max_matches=%u, variables) => %u",
                                                  name.GetCString(),
                                                  static_cast<const void*>(parent_decl_ctx),
                                                  append, max_matches,
                                                  num_matches);
    }
    return num_matches;
}

uint32_t
SymbolFileDWARF::FindGlobalVariables(const RegularExpression& regex, bool append, uint32_t max_matches, VariableList& variables)
{
    Log *log (LogChannelDWARF::GetLogIfAll(DWARF_LOG_LOOKUPS));

    if (log)
    {
        GetObjectFile()->GetModule()->LogMessage (log,
                                                  "SymbolFileDWARF::FindGlobalVariables (regex=\"%s\", append=%u, max_matches=%u, variables)", 
                                                  regex.GetText(), append,
                                                  max_matches);
    }

    DWARFDebugInfo* info = DebugInfo();
    if (info == NULL)
        return 0;

    // If we aren't appending the results to this list, then clear the list
    if (!append)
        variables.Clear();

    // Remember how many variables are in the list before we search in case
    // we are appending the results to a variable list.
    const uint32_t original_size = variables.GetSize();

    DIEArray die_offsets;

    if (m_using_apple_tables)
    {
        if (m_apple_names_ap.get())
        {
            DWARFMappedHash::DIEInfoArray hash_data_array;
            if (m_apple_names_ap->AppendAllDIEsThatMatchingRegex (regex, hash_data_array))
                DWARFMappedHash::ExtractDIEArray (hash_data_array, die_offsets);
        }
    }
    else
    {
        // Index the DWARF if we haven't already
        if (!m_indexed)
            Index ();
        
        m_global_index.Find (regex, die_offsets);
    }

    SymbolContext sc;
    sc.module_sp = m_obj_file->GetModule();
    assert (sc.module_sp);
    
    const size_t num_matches = die_offsets.size();
    if (num_matches)
    {
        DWARFDebugInfo* debug_info = DebugInfo();
        for (size_t i=0; i<num_matches; ++i)
        {
            const DIERef& die_ref = die_offsets[i];
            DWARFDIE die = debug_info->GetDIE (die_ref);
            
            if (die)
            {
                sc.comp_unit = GetCompUnitForDWARFCompUnit(die.GetCU(), UINT32_MAX);

                ParseVariables(sc, die, LLDB_INVALID_ADDRESS, false, false, &variables);

                if (variables.GetSize() - original_size >= max_matches)
                    break;
            }
            else
            {
                if (m_using_apple_tables)
                {
                    GetObjectFile()->GetModule()->ReportErrorIfModifyDetected ("the DWARF debug information has been modified (.apple_names accelerator table had bad die 0x%8.8x for regex '%s')\n",
                                                                               die_ref.die_offset, regex.GetText());
                }
            }            
        }
    }

    // Return the number of variable that were appended to the list
    return variables.GetSize() - original_size;
}


bool
SymbolFileDWARF::ResolveFunction (const DIERef& die_ref,
                                  bool include_inlines,
                                  SymbolContextList& sc_list)
{
    DWARFDIE die = DebugInfo()->GetDIE (die_ref);
    return ResolveFunction (die, include_inlines, sc_list);
}
    

bool
SymbolFileDWARF::ResolveFunction (const DWARFDIE &orig_die,
                                  bool include_inlines,
                                  SymbolContextList& sc_list)
{
    SymbolContext sc;

    if (!orig_die)
        return false;

    // If we were passed a die that is not a function, just return false...
    if (!(orig_die.Tag() == DW_TAG_subprogram || (include_inlines && orig_die.Tag() == DW_TAG_inlined_subroutine)))
        return false;

    DWARFDIE die = orig_die;
    DWARFDIE inlined_die;
    if (die.Tag() == DW_TAG_inlined_subroutine)
    {
        inlined_die = die;
        
        while (1)
        {
            die = die.GetParent();

            if (die)
            {
                if (die.Tag() == DW_TAG_subprogram)
                    break;
            }
            else
                break;
        }
    }
    assert (die && die.Tag() == DW_TAG_subprogram);
    if (GetFunction (die, sc))
    {
        Address addr;
        // Parse all blocks if needed
        if (inlined_die)
        {
            Block &function_block = sc.function->GetBlock (true);
            sc.block = function_block.FindBlockByID (inlined_die.GetID());
            if (sc.block == NULL)
                sc.block = function_block.FindBlockByID (inlined_die.GetOffset());
            if (sc.block == NULL || sc.block->GetStartAddress (addr) == false)
                addr.Clear();
        }
        else 
        {
            sc.block = NULL;
            addr = sc.function->GetAddressRange().GetBaseAddress();
        }

        if (addr.IsValid())
        {
            sc_list.Append(sc);
            return true;
        }
    }
    
    return false;
}

void
SymbolFileDWARF::FindFunctions (const ConstString &name, 
                                const NameToDIE &name_to_die,
                                bool include_inlines,
                                SymbolContextList& sc_list)
{
    DIEArray die_offsets;
    if (name_to_die.Find (name, die_offsets))
    {
        ParseFunctions (die_offsets, include_inlines, sc_list);
    }
}


void
SymbolFileDWARF::FindFunctions (const RegularExpression &regex, 
                                const NameToDIE &name_to_die,
                                bool include_inlines,
                                SymbolContextList& sc_list)
{
    DIEArray die_offsets;
    if (name_to_die.Find (regex, die_offsets))
    {
        ParseFunctions (die_offsets, include_inlines, sc_list);
    }
}


void
SymbolFileDWARF::FindFunctions (const RegularExpression &regex, 
                                const DWARFMappedHash::MemoryTable &memory_table,
                                bool include_inlines,
                                SymbolContextList& sc_list)
{
    DIEArray die_offsets;
    DWARFMappedHash::DIEInfoArray hash_data_array;
    if (memory_table.AppendAllDIEsThatMatchingRegex (regex, hash_data_array))
    {
        DWARFMappedHash::ExtractDIEArray (hash_data_array, die_offsets);
        ParseFunctions (die_offsets, include_inlines, sc_list);
    }
}

void
SymbolFileDWARF::ParseFunctions (const DIEArray &die_offsets,
                                 bool include_inlines,
                                 SymbolContextList& sc_list)
{
    const size_t num_matches = die_offsets.size();
    if (num_matches)
    {
        for (size_t i=0; i<num_matches; ++i)
            ResolveFunction (die_offsets[i], include_inlines, sc_list);
    }
}

bool
SymbolFileDWARF::DIEInDeclContext (const CompilerDeclContext *decl_ctx,
                                   const DWARFDIE &die)
{
    // If we have no parent decl context to match this DIE matches, and if the parent
    // decl context isn't valid, we aren't trying to look for any particular decl
    // context so any die matches.
    if (decl_ctx == nullptr || !decl_ctx->IsValid())
        return true;

    if (die)
    {
        DWARFASTParser *dwarf_ast = die.GetDWARFParser();
        if (dwarf_ast)
        {
            CompilerDeclContext actual_decl_ctx = dwarf_ast->GetDeclContextContainingUIDFromDWARF (die);
            if (actual_decl_ctx)
                return actual_decl_ctx == *decl_ctx;
        }
    }
    return false;
}

uint32_t
SymbolFileDWARF::FindFunctions (const ConstString &name,
                                const CompilerDeclContext *parent_decl_ctx,
                                uint32_t name_type_mask,
                                bool include_inlines,
                                bool append, 
                                SymbolContextList& sc_list)
{
    Timer scoped_timer (__PRETTY_FUNCTION__,
                        "SymbolFileDWARF::FindFunctions (name = '%s')",
                        name.AsCString());

    // eFunctionNameTypeAuto should be pre-resolved by a call to Module::PrepareForFunctionNameLookup()
    assert ((name_type_mask & eFunctionNameTypeAuto) == 0);

    Log *log (LogChannelDWARF::GetLogIfAll(DWARF_LOG_LOOKUPS));
    
    if (log)
    {
        GetObjectFile()->GetModule()->LogMessage (log,
                                                  "SymbolFileDWARF::FindFunctions (name=\"%s\", name_type_mask=0x%x, append=%u, sc_list)", 
                                                  name.GetCString(), 
                                                  name_type_mask, 
                                                  append);
    }

    // If we aren't appending the results to this list, then clear the list
    if (!append)
        sc_list.Clear();
    
    if (!DeclContextMatchesThisSymbolFile(parent_decl_ctx))
        return 0;
        
    // If name is empty then we won't find anything.
    if (name.IsEmpty())
        return 0;

    // Remember how many sc_list are in the list before we search in case
    // we are appending the results to a variable list.

    const char *name_cstr = name.GetCString();

    const uint32_t original_size = sc_list.GetSize();
   
    DWARFDebugInfo* info = DebugInfo();
    if (info == NULL)
        return 0;

    std::set<const DWARFDebugInfoEntry *> resolved_dies;
    if (m_using_apple_tables)
    {
        if (m_apple_names_ap.get())
        {

            DIEArray die_offsets;

            uint32_t num_matches = 0;
                
            if (name_type_mask & eFunctionNameTypeFull)
            {
                // If they asked for the full name, match what they typed.  At some point we may
                // want to canonicalize this (strip double spaces, etc.  For now, we just add all the
                // dies that we find by exact match.
                num_matches = m_apple_names_ap->FindByName (name_cstr, die_offsets);
                for (uint32_t i = 0; i < num_matches; i++)
                {
                    const DIERef& die_ref = die_offsets[i];
                    DWARFDIE die = info->GetDIE (die_ref);
                    if (die)
                    {
                        if (!DIEInDeclContext(parent_decl_ctx, die))
                            continue; // The containing decl contexts don't match

                        if (resolved_dies.find(die.GetDIE()) == resolved_dies.end())
                        {
                            if (ResolveFunction (die, include_inlines, sc_list))
                                resolved_dies.insert(die.GetDIE());
                        }
                    }
                    else
                    {
                        GetObjectFile()->GetModule()->ReportErrorIfModifyDetected ("the DWARF debug information has been modified (.apple_names accelerator table had bad die 0x%8.8x for '%s')", 
                                                                                   die_ref.die_offset, name_cstr);
                    }                                    
                }
            }

            if (name_type_mask & eFunctionNameTypeSelector)
            {
                if (parent_decl_ctx && parent_decl_ctx->IsValid())
                    return 0; // no selectors in namespaces
                    
                num_matches = m_apple_names_ap->FindByName (name_cstr, die_offsets);
                // Now make sure these are actually ObjC methods.  In this case we can simply look up the name,
                // and if it is an ObjC method name, we're good.
                
                for (uint32_t i = 0; i < num_matches; i++)
                {
                    const DIERef& die_ref = die_offsets[i];
                    DWARFDIE die = info->GetDIE (die_ref);
                    if (die)
                    {
                        const char *die_name = die.GetName();
                        if (ObjCLanguage::IsPossibleObjCMethodName(die_name))
                        {
                            if (resolved_dies.find(die.GetDIE()) == resolved_dies.end())
                            {
                                if (ResolveFunction (die, include_inlines, sc_list))
                                    resolved_dies.insert(die.GetDIE());
                            }
                        }
                    }
                    else
                    {
                        GetObjectFile()->GetModule()->ReportError ("the DWARF debug information has been modified (.apple_names accelerator table had bad die 0x%8.8x for '%s')",
                                                                   die_ref.die_offset, name_cstr);
                    }                                    
                }
                die_offsets.clear();
            }
            
            if (((name_type_mask & eFunctionNameTypeMethod) && !parent_decl_ctx) || name_type_mask & eFunctionNameTypeBase)
            {
                // The apple_names table stores just the "base name" of C++ methods in the table.  So we have to
                // extract the base name, look that up, and if there is any other information in the name we were
                // passed in we have to post-filter based on that.
                
                // FIXME: Arrange the logic above so that we don't calculate the base name twice:
                num_matches = m_apple_names_ap->FindByName (name_cstr, die_offsets);
                
                for (uint32_t i = 0; i < num_matches; i++)
                {
                    const DIERef& die_ref = die_offsets[i];
                    DWARFDIE die = info->GetDIE (die_ref);
                    if (die)
                    {
                        if (!DIEInDeclContext(parent_decl_ctx, die))
                            continue; // The containing decl contexts don't match


                        // If we get to here, the die is good, and we should add it:
                        if (resolved_dies.find(die.GetDIE()) == resolved_dies.end() && ResolveFunction (die, include_inlines, sc_list))
                        {
                            bool keep_die = true;
                            if ((name_type_mask & (eFunctionNameTypeBase|eFunctionNameTypeMethod)) != (eFunctionNameTypeBase|eFunctionNameTypeMethod))
                            {
                                // We are looking for either basenames or methods, so we need to
                                // trim out the ones we won't want by looking at the type
                                SymbolContext sc;
                                if (sc_list.GetLastContext(sc))
                                {
                                    if (sc.block)
                                    {
                                        // We have an inlined function
                                    }
                                    else if (sc.function)
                                    {
                                        Type *type = sc.function->GetType();
                                        
                                        if (type)
                                        {
                                            CompilerDeclContext decl_ctx = GetDeclContextContainingUID (type->GetID());
                                            if (decl_ctx.IsStructUnionOrClass())
                                            {
                                                if (name_type_mask & eFunctionNameTypeBase)
                                                {
                                                    sc_list.RemoveContextAtIndex(sc_list.GetSize()-1);
                                                    keep_die = false;
                                                }
                                            }
                                            else
                                            {
                                                if (name_type_mask & eFunctionNameTypeMethod)
                                                {
                                                    sc_list.RemoveContextAtIndex(sc_list.GetSize()-1);
                                                    keep_die = false;
                                                }
                                            }
                                        }
                                        else
                                        {
                                            GetObjectFile()->GetModule()->ReportWarning ("function at die offset 0x%8.8x had no function type",
                                                                                         die_ref.die_offset);
                                        }
                                    }
                                }
                            }
                            if (keep_die)
                                resolved_dies.insert(die.GetDIE());
                        }
                    }
                    else
                    {
                        GetObjectFile()->GetModule()->ReportErrorIfModifyDetected ("the DWARF debug information has been modified (.apple_names accelerator table had bad die 0x%8.8x for '%s')",
                                                                                   die_ref.die_offset, name_cstr);
                    }                                    
                }
                die_offsets.clear();
            }
        }
    }
    else
    {

        // Index the DWARF if we haven't already
        if (!m_indexed)
            Index ();

        if (name_type_mask & eFunctionNameTypeFull)
        {
            FindFunctions (name, m_function_fullname_index, include_inlines, sc_list);

            // FIXME Temporary workaround for global/anonymous namespace
            // functions debugging FreeBSD and Linux binaries.
            // If we didn't find any functions in the global namespace try
            // looking in the basename index but ignore any returned
            // functions that have a namespace but keep functions which
            // have an anonymous namespace
            // TODO: The arch in the object file isn't correct for MSVC
            // binaries on windows, we should find a way to make it
            // correct and handle those symbols as well.
            if (sc_list.GetSize() == original_size)
            {
                ArchSpec arch;
                if (!parent_decl_ctx &&
                    GetObjectFile()->GetArchitecture(arch) &&
                    (arch.GetTriple().isOSFreeBSD() || arch.GetTriple().isOSLinux() ||
                     arch.GetMachine() == llvm::Triple::hexagon))
                {
                    SymbolContextList temp_sc_list;
                    FindFunctions (name, m_function_basename_index, include_inlines, temp_sc_list);
                    SymbolContext sc;
                    for (uint32_t i = 0; i < temp_sc_list.GetSize(); i++)
                    {
                        if (temp_sc_list.GetContextAtIndex(i, sc))
                        {
                            ConstString mangled_name = sc.GetFunctionName(Mangled::ePreferMangled);
                            ConstString demangled_name = sc.GetFunctionName(Mangled::ePreferDemangled);
                            // Mangled names on Linux and FreeBSD are of the form:
                            // _ZN18function_namespace13function_nameEv.
                            if (strncmp(mangled_name.GetCString(), "_ZN", 3) ||
                                !strncmp(demangled_name.GetCString(), "(anonymous namespace)", 21))
                            {
                                sc_list.Append(sc);
                            }
                        }
                    }
                }
            }
        }
        DIEArray die_offsets;
        if (name_type_mask & eFunctionNameTypeBase)
        {
            uint32_t num_base = m_function_basename_index.Find(name, die_offsets);
            for (uint32_t i = 0; i < num_base; i++)
            {
                DWARFDIE die = info->GetDIE (die_offsets[i]);
                if (die)
                {
                    if (!DIEInDeclContext(parent_decl_ctx, die))
                        continue; // The containing decl contexts don't match

                    // If we get to here, the die is good, and we should add it:
                    if (resolved_dies.find(die.GetDIE()) == resolved_dies.end())
                    {
                        if (ResolveFunction (die, include_inlines, sc_list))
                            resolved_dies.insert(die.GetDIE());
                    }
                }
            }
            die_offsets.clear();
        }
        
        if (name_type_mask & eFunctionNameTypeMethod)
        {
            if (parent_decl_ctx && parent_decl_ctx->IsValid())
                return 0; // no methods in namespaces

            uint32_t num_base = m_function_method_index.Find(name, die_offsets);
            {
                for (uint32_t i = 0; i < num_base; i++)
                {
                    DWARFDIE die = info->GetDIE (die_offsets[i]);
                    if (die)
                    {
                        // If we get to here, the die is good, and we should add it:
                        if (resolved_dies.find(die.GetDIE()) == resolved_dies.end())
                        {
                            if (ResolveFunction (die, include_inlines, sc_list))
                                resolved_dies.insert(die.GetDIE());
                        }
                    }
                }
            }
            die_offsets.clear();
        }

        if ((name_type_mask & eFunctionNameTypeSelector) && (!parent_decl_ctx || !parent_decl_ctx->IsValid()))
        {
            FindFunctions (name, m_function_selector_index, include_inlines, sc_list);
        }
        
    }

    // Return the number of variable that were appended to the list
    const uint32_t num_matches = sc_list.GetSize() - original_size;
    
    if (log && num_matches > 0)
    {
        GetObjectFile()->GetModule()->LogMessage (log,
                                                  "SymbolFileDWARF::FindFunctions (name=\"%s\", name_type_mask=0x%x, include_inlines=%d, append=%u, sc_list) => %u",
                                                  name.GetCString(), 
                                                  name_type_mask, 
                                                  include_inlines,
                                                  append,
                                                  num_matches);
    }
    return num_matches;
}

uint32_t
SymbolFileDWARF::FindFunctions(const RegularExpression& regex, bool include_inlines, bool append, SymbolContextList& sc_list)
{
    Timer scoped_timer (__PRETTY_FUNCTION__,
                        "SymbolFileDWARF::FindFunctions (regex = '%s')",
                        regex.GetText());

    Log *log (LogChannelDWARF::GetLogIfAll(DWARF_LOG_LOOKUPS));
    
    if (log)
    {
        GetObjectFile()->GetModule()->LogMessage (log,
                                                  "SymbolFileDWARF::FindFunctions (regex=\"%s\", append=%u, sc_list)", 
                                                  regex.GetText(), 
                                                  append);
    }
    

    // If we aren't appending the results to this list, then clear the list
    if (!append)
        sc_list.Clear();

    // Remember how many sc_list are in the list before we search in case
    // we are appending the results to a variable list.
    uint32_t original_size = sc_list.GetSize();

    if (m_using_apple_tables)
    {
        if (m_apple_names_ap.get())
            FindFunctions (regex, *m_apple_names_ap, include_inlines, sc_list);
    }
    else
    {
        // Index the DWARF if we haven't already
        if (!m_indexed)
            Index ();

        FindFunctions (regex, m_function_basename_index, include_inlines, sc_list);

        FindFunctions (regex, m_function_fullname_index, include_inlines, sc_list);
    }

    // Return the number of variable that were appended to the list
    return sc_list.GetSize() - original_size;
}

void
SymbolFileDWARF::GetMangledNamesForFunction (const std::string &scope_qualified_name,
                                             std::vector<ConstString> &mangled_names)
{
    DWARFDebugInfo* info = DebugInfo();
    uint32_t num_comp_units = 0;
    if (info)
        num_comp_units = info->GetNumCompileUnits();

    for (uint32_t i = 0; i < num_comp_units; i++)
    {
        DWARFCompileUnit *cu = info->GetCompileUnitAtIndex(i);
        if (cu == nullptr)
            continue;

        SymbolFileDWARFDwo *dwo = cu->GetDwoSymbolFile();
        if (dwo)
            dwo->GetMangledNamesForFunction(scope_qualified_name, mangled_names);
    }

    NameToOffsetMap::iterator iter = m_function_scope_qualified_name_map.find(scope_qualified_name);
    if (iter == m_function_scope_qualified_name_map.end())
        return;

    DIERefSetSP set_sp = (*iter).second;
    std::set<DIERef>::iterator set_iter;
    for (set_iter = set_sp->begin(); set_iter != set_sp->end(); set_iter++)
    {
        DWARFDIE die = DebugInfo()->GetDIE (*set_iter);
        mangled_names.push_back(ConstString(die.GetMangledName()));
    }
}


uint32_t
SymbolFileDWARF::FindTypes (const SymbolContext& sc, 
                            const ConstString &name, 
                            const CompilerDeclContext *parent_decl_ctx, 
                            bool append, 
                            uint32_t max_matches,
                            llvm::DenseSet<lldb_private::SymbolFile *> &searched_symbol_files,
                            TypeMap& types)
{
    // If we aren't appending the results to this list, then clear the list
    if (!append)
        types.Clear();

    // Make sure we haven't already searched this SymbolFile before...
    if (searched_symbol_files.count(this))
        return 0;
    else
        searched_symbol_files.insert(this);

    DWARFDebugInfo* info = DebugInfo();
    if (info == NULL)
        return 0;

    Log *log (LogChannelDWARF::GetLogIfAll(DWARF_LOG_LOOKUPS));

    if (log)
    {
        if (parent_decl_ctx)
            GetObjectFile()->GetModule()->LogMessage (log,
                                                      "SymbolFileDWARF::FindTypes (sc, name=\"%s\", parent_decl_ctx = %p (\"%s\"), append=%u, max_matches=%u, type_list)",
                                                      name.GetCString(),
                                                      static_cast<const void*>(parent_decl_ctx),
                                                      parent_decl_ctx->GetName().AsCString("<NULL>"),
                                                      append, max_matches);
        else
            GetObjectFile()->GetModule()->LogMessage (log,
                                                      "SymbolFileDWARF::FindTypes (sc, name=\"%s\", parent_decl_ctx = NULL, append=%u, max_matches=%u, type_list)",
                                                      name.GetCString(), append,
                                                      max_matches);
    }

    if (!DeclContextMatchesThisSymbolFile(parent_decl_ctx))
        return 0;

    DIEArray die_offsets;

    if (m_using_apple_tables)
    {
        if (m_apple_types_ap.get())
        {
            const char *name_cstr = name.GetCString();
            m_apple_types_ap->FindByName (name_cstr, die_offsets);
        }
    }
    else
    {
        if (!m_indexed)
            Index ();

        m_type_index.Find (name, die_offsets);
    }

    const size_t num_die_matches = die_offsets.size();

    if (num_die_matches)
    {
        const uint32_t initial_types_size = types.GetSize();
        DWARFDebugInfo* debug_info = DebugInfo();
        for (size_t i=0; i<num_die_matches; ++i)
        {
            const DIERef& die_ref = die_offsets[i];
            DWARFDIE die = debug_info->GetDIE (die_ref);

            if (die)
            {
                if (!DIEInDeclContext(parent_decl_ctx, die))
                    continue; // The containing decl contexts don't match

                Type *matching_type = ResolveType (die, true, true);
                if (matching_type)
                {
                    // We found a type pointer, now find the shared pointer form our type list
                    types.InsertUnique (matching_type->shared_from_this());
                    if (types.GetSize() >= max_matches)
                        break;
                }
            }
            else
            {
                if (m_using_apple_tables)
                {
                    GetObjectFile()->GetModule()->ReportErrorIfModifyDetected ("the DWARF debug information has been modified (.apple_types accelerator table had bad die 0x%8.8x for '%s')\n",
                                                                               die_ref.die_offset, name.GetCString());
                }
            }            

        }
        const uint32_t num_matches = types.GetSize() - initial_types_size;
        if (log && num_matches)
        {
            if (parent_decl_ctx)
            {
                GetObjectFile()->GetModule()->LogMessage (log,
                                                          "SymbolFileDWARF::FindTypes (sc, name=\"%s\", parent_decl_ctx = %p (\"%s\"), append=%u, max_matches=%u, type_list) => %u",
                                                          name.GetCString(),
                                                          static_cast<const void*>(parent_decl_ctx),
                                                          parent_decl_ctx->GetName().AsCString("<NULL>"),
                                                          append, max_matches,
                                                          num_matches);
            }
            else
            {
                GetObjectFile()->GetModule()->LogMessage (log,
                                                          "SymbolFileDWARF::FindTypes (sc, name=\"%s\", parent_decl_ctx = NULL, append=%u, max_matches=%u, type_list) => %u",
                                                          name.GetCString(), 
                                                          append, max_matches,
                                                          num_matches);
            }
        }
        return num_matches;
    }
    else
    {
        UpdateExternalModuleListIfNeeded();

        for (const auto &pair : m_external_type_modules)
        {
            ModuleSP external_module_sp = pair.second;
            if (external_module_sp)
            {
                SymbolVendor *sym_vendor = external_module_sp->GetSymbolVendor();
                if (sym_vendor)
                {
                    const uint32_t num_external_matches = sym_vendor->FindTypes (sc,
                                                                                 name,
                                                                                 parent_decl_ctx,
                                                                                 append,
                                                                                 max_matches,
                                                                                 searched_symbol_files,
                                                                                 types);
                    if (num_external_matches)
                        return num_external_matches;
                }
            }
        }
    }

    return 0;
}


size_t
SymbolFileDWARF::FindTypes (const std::vector<CompilerContext> &context,
                            bool append,
                            TypeMap& types)
{
    if (!append)
        types.Clear();

    if (context.empty())
        return 0;

    DIEArray die_offsets;

    ConstString name = context.back().name;

    if (!name)
        return 0;

    if (m_using_apple_tables)
    {
        if (m_apple_types_ap.get())
        {
            const char *name_cstr = name.GetCString();
            m_apple_types_ap->FindByName (name_cstr, die_offsets);
        }
    }
    else
    {
        if (!m_indexed)
            Index ();

        m_type_index.Find (name, die_offsets);
    }

    const size_t num_die_matches = die_offsets.size();

    if (num_die_matches)
    {
        size_t num_matches = 0;
        DWARFDebugInfo* debug_info = DebugInfo();
        for (size_t i=0; i<num_die_matches; ++i)
        {
            const DIERef& die_ref = die_offsets[i];
            DWARFDIE die = debug_info->GetDIE (die_ref);

            if (die)
            {
                std::vector<CompilerContext> die_context;
                die.GetDWOContext(die_context);
                if (die_context != context)
                    continue;

                Type *matching_type = ResolveType (die, true, true);
                if (matching_type)
                {
                    // We found a type pointer, now find the shared pointer form our type list
                    types.InsertUnique (matching_type->shared_from_this());
                    ++num_matches;
                }
            }
            else
            {
                if (m_using_apple_tables)
                {
                    GetObjectFile()->GetModule()->ReportErrorIfModifyDetected ("the DWARF debug information has been modified (.apple_types accelerator table had bad die 0x%8.8x for '%s')\n",
                                                                               die_ref.die_offset, name.GetCString());
                }
            }

        }
        return num_matches;
    }
    return 0;
}


CompilerDeclContext
SymbolFileDWARF::FindNamespace (const SymbolContext& sc, 
                                const ConstString &name,
                                const CompilerDeclContext *parent_decl_ctx)
{
    Log *log (LogChannelDWARF::GetLogIfAll(DWARF_LOG_LOOKUPS));
    
    if (log)
    {
        GetObjectFile()->GetModule()->LogMessage (log,
                                                  "SymbolFileDWARF::FindNamespace (sc, name=\"%s\")", 
                                                  name.GetCString());
    }

    CompilerDeclContext namespace_decl_ctx;

    if (!DeclContextMatchesThisSymbolFile(parent_decl_ctx))
        return namespace_decl_ctx;


    DWARFDebugInfo* info = DebugInfo();
    if (info)
    {
        DIEArray die_offsets;

        // Index if we already haven't to make sure the compile units
        // get indexed and make their global DIE index list
        if (m_using_apple_tables)
        {
            if (m_apple_namespaces_ap.get())
            {
                const char *name_cstr = name.GetCString();
                m_apple_namespaces_ap->FindByName (name_cstr, die_offsets);
            }
        }
        else
        {
            if (!m_indexed)
                Index ();

            m_namespace_index.Find (name, die_offsets);
        }
        
        const size_t num_matches = die_offsets.size();
        if (num_matches)
        {
            DWARFDebugInfo* debug_info = DebugInfo();
            for (size_t i=0; i<num_matches; ++i)
            {
                const DIERef& die_ref = die_offsets[i];
                DWARFDIE die = debug_info->GetDIE (die_ref);

                if (die)
                {
                    if (!DIEInDeclContext (parent_decl_ctx, die))
                        continue; // The containing decl contexts don't match

                    DWARFASTParser *dwarf_ast = die.GetDWARFParser();
                    if (dwarf_ast)
                    {
                        namespace_decl_ctx = dwarf_ast->GetDeclContextForUIDFromDWARF (die);
                        if (namespace_decl_ctx)
                            break;
                    }
                }
                else
                {
                    if (m_using_apple_tables)
                    {
                        GetObjectFile()->GetModule()->ReportErrorIfModifyDetected ("the DWARF debug information has been modified (.apple_namespaces accelerator table had bad die 0x%8.8x for '%s')\n",
                                                                   die_ref.die_offset, name.GetCString());
                    }
                }            

            }
        }
    }
    if (log && namespace_decl_ctx)
    {
        GetObjectFile()->GetModule()->LogMessage (log,
                                                  "SymbolFileDWARF::FindNamespace (sc, name=\"%s\") => CompilerDeclContext(%p/%p) \"%s\"",
                                                  name.GetCString(),
                                                  static_cast<const void*>(namespace_decl_ctx.GetTypeSystem()),
                                                  static_cast<const void*>(namespace_decl_ctx.GetOpaqueDeclContext()),
                                                  namespace_decl_ctx.GetName().AsCString("<NULL>"));
    }

    return namespace_decl_ctx;
}

TypeSP
SymbolFileDWARF::GetTypeForDIE (const DWARFDIE &die, bool resolve_function_context)
{
    TypeSP type_sp;
    if (die)
    {
        Type *type_ptr = GetDIEToType().lookup (die.GetDIE());
        if (type_ptr == NULL)
        {
            CompileUnit* lldb_cu = GetCompUnitForDWARFCompUnit(die.GetCU());
            assert (lldb_cu);
            SymbolContext sc(lldb_cu);
            const DWARFDebugInfoEntry* parent_die = die.GetParent().GetDIE();
            while (parent_die != nullptr)
                {
                    if (parent_die->Tag() == DW_TAG_subprogram)
                        break;
                    parent_die = parent_die->GetParent();
                }
            SymbolContext sc_backup = sc;
            if (resolve_function_context && parent_die != nullptr && !GetFunction(DWARFDIE(die.GetCU(),parent_die), sc))
                sc = sc_backup;

            type_sp = ParseType(sc, die, NULL);
        }
        else if (type_ptr != DIE_IS_BEING_PARSED)
        {
            // Grab the existing type from the master types lists
            type_sp = type_ptr->shared_from_this();
        }

    }
    return type_sp;
}


DWARFDIE
SymbolFileDWARF::GetDeclContextDIEContainingDIE (const DWARFDIE &orig_die)
{
    if (orig_die)
    {
        DWARFDIE die = orig_die;
    
        while (die)
        {
            // If this is the original DIE that we are searching for a declaration 
            // for, then don't look in the cache as we don't want our own decl 
            // context to be our decl context...
            if (orig_die != die)
            {            
                switch (die.Tag())
                {
                    case DW_TAG_compile_unit:
                    case DW_TAG_namespace:
                    case DW_TAG_structure_type:
                    case DW_TAG_union_type:
                    case DW_TAG_class_type:
                    case DW_TAG_lexical_block:
                    case DW_TAG_subprogram:
                        return die;
                        
                    default:
                        break;
                }
            }

            DWARFDIE spec_die = die.GetReferencedDIE(DW_AT_specification);
            if (spec_die)
            {
                DWARFDIE decl_ctx_die = GetDeclContextDIEContainingDIE(spec_die);
                if (decl_ctx_die)
                    return decl_ctx_die;
            }

            DWARFDIE abs_die = die.GetReferencedDIE(DW_AT_abstract_origin);
            if (abs_die)
            {
                DWARFDIE decl_ctx_die = GetDeclContextDIEContainingDIE(abs_die);
                if (decl_ctx_die)
                    return decl_ctx_die;
            }

            die = die.GetParent();
        }
    }
    return DWARFDIE();
}


Symbol *
SymbolFileDWARF::GetObjCClassSymbol (const ConstString &objc_class_name)
{
    Symbol *objc_class_symbol = NULL;
    if (m_obj_file)
    {
        Symtab *symtab = m_obj_file->GetSymtab ();
        if (symtab)
        {
            objc_class_symbol = symtab->FindFirstSymbolWithNameAndType (objc_class_name, 
                                                                        eSymbolTypeObjCClass, 
                                                                        Symtab::eDebugNo, 
                                                                        Symtab::eVisibilityAny);
        }
    }
    return objc_class_symbol;
}

// Some compilers don't emit the DW_AT_APPLE_objc_complete_type attribute. If they don't
// then we can end up looking through all class types for a complete type and never find
// the full definition. We need to know if this attribute is supported, so we determine
// this here and cache th result. We also need to worry about the debug map DWARF file
// if we are doing darwin DWARF in .o file debugging.
bool
SymbolFileDWARF::Supports_DW_AT_APPLE_objc_complete_type (DWARFCompileUnit *cu)
{
    if (m_supports_DW_AT_APPLE_objc_complete_type == eLazyBoolCalculate)
    {
        m_supports_DW_AT_APPLE_objc_complete_type = eLazyBoolNo;
        if (cu && cu->Supports_DW_AT_APPLE_objc_complete_type())
            m_supports_DW_AT_APPLE_objc_complete_type = eLazyBoolYes;
        else
        {
            DWARFDebugInfo* debug_info = DebugInfo();
            const uint32_t num_compile_units = GetNumCompileUnits();
            for (uint32_t cu_idx = 0; cu_idx < num_compile_units; ++cu_idx)
            {
                DWARFCompileUnit* dwarf_cu = debug_info->GetCompileUnitAtIndex(cu_idx);
                if (dwarf_cu != cu && dwarf_cu->Supports_DW_AT_APPLE_objc_complete_type())
                {
                    m_supports_DW_AT_APPLE_objc_complete_type = eLazyBoolYes;
                    break;
                }
            }
        }
        if (m_supports_DW_AT_APPLE_objc_complete_type == eLazyBoolNo && GetDebugMapSymfile ())
            return m_debug_map_symfile->Supports_DW_AT_APPLE_objc_complete_type (this);
    }
    return m_supports_DW_AT_APPLE_objc_complete_type == eLazyBoolYes;
}

// This function can be used when a DIE is found that is a forward declaration
// DIE and we want to try and find a type that has the complete definition.
TypeSP
SymbolFileDWARF::FindCompleteObjCDefinitionTypeForDIE (const DWARFDIE &die,
                                                       const ConstString &type_name,
                                                       bool must_be_implementation)
{
    
    TypeSP type_sp;
    
    if (!type_name || (must_be_implementation && !GetObjCClassSymbol (type_name)))
        return type_sp;
    
    DIEArray die_offsets;
    
    if (m_using_apple_tables)
    {
        if (m_apple_types_ap.get())
        {
            const char *name_cstr = type_name.GetCString();
            m_apple_types_ap->FindCompleteObjCClassByName (name_cstr, die_offsets, must_be_implementation);
        }
    }
    else
    {
        if (!m_indexed)
            Index ();
        
        m_type_index.Find (type_name, die_offsets);
    }
    
    const size_t num_matches = die_offsets.size();
    
    if (num_matches)
    {
        DWARFDebugInfo* debug_info = DebugInfo();
        for (size_t i=0; i<num_matches; ++i)
        {
            const DIERef& die_ref = die_offsets[i];
            DWARFDIE type_die = debug_info->GetDIE (die_ref);
            
            if (type_die)
            {
                bool try_resolving_type = false;
                
                // Don't try and resolve the DIE we are looking for with the DIE itself!
                if (type_die != die)
                {
                    switch (type_die.Tag())
                    {
                        case DW_TAG_class_type:
                        case DW_TAG_structure_type:
                            try_resolving_type = true;
                            break;
                        default:
                            break;
                    }
                }
                
                if (try_resolving_type)
                {
                    if (must_be_implementation && type_die.Supports_DW_AT_APPLE_objc_complete_type())
                        try_resolving_type = type_die.GetAttributeValueAsUnsigned (DW_AT_APPLE_objc_complete_type, 0);
                    
                    if (try_resolving_type)
                    {
                        Type *resolved_type = ResolveType (type_die, false, true);
                        if (resolved_type && resolved_type != DIE_IS_BEING_PARSED)
                        {
                            DEBUG_PRINTF ("resolved 0x%8.8" PRIx64 " from %s to 0x%8.8" PRIx64 " (cu 0x%8.8" PRIx64 ")\n",
                                          die.GetID(),
                                          m_obj_file->GetFileSpec().GetFilename().AsCString("<Unknown>"),
                                          type_die.GetID(),
                                          type_cu->GetID());
                            
                            if (die)
                                GetDIEToType()[die.GetDIE()] = resolved_type;
                            type_sp = resolved_type->shared_from_this();
                            break;
                        }
                    }
                }
            }
            else
            {
                if (m_using_apple_tables)
                {
                    GetObjectFile()->GetModule()->ReportErrorIfModifyDetected ("the DWARF debug information has been modified (.apple_types accelerator table had bad die 0x%8.8x for '%s')\n",
                                                               die_ref.die_offset, type_name.GetCString());
                }
            }            
            
        }
    }
    return type_sp;
}


//----------------------------------------------------------------------
// This function helps to ensure that the declaration contexts match for
// two different DIEs. Often times debug information will refer to a 
// forward declaration of a type (the equivalent of "struct my_struct;".
// There will often be a declaration of that type elsewhere that has the
// full definition. When we go looking for the full type "my_struct", we
// will find one or more matches in the accelerator tables and we will
// then need to make sure the type was in the same declaration context 
// as the original DIE. This function can efficiently compare two DIEs
// and will return true when the declaration context matches, and false
// when they don't. 
//----------------------------------------------------------------------
bool
SymbolFileDWARF::DIEDeclContextsMatch (const DWARFDIE &die1,
                                       const DWARFDIE &die2)
{
    if (die1 == die2)
        return true;

    DWARFDIECollection decl_ctx_1;
    DWARFDIECollection decl_ctx_2;
    //The declaration DIE stack is a stack of the declaration context 
    // DIEs all the way back to the compile unit. If a type "T" is
    // declared inside a class "B", and class "B" is declared inside
    // a class "A" and class "A" is in a namespace "lldb", and the
    // namespace is in a compile unit, there will be a stack of DIEs:
    //
    //   [0] DW_TAG_class_type for "B"
    //   [1] DW_TAG_class_type for "A"
    //   [2] DW_TAG_namespace  for "lldb"
    //   [3] DW_TAG_compile_unit for the source file.
    // 
    // We grab both contexts and make sure that everything matches 
    // all the way back to the compiler unit.
    
    // First lets grab the decl contexts for both DIEs
    die1.GetDeclContextDIEs (decl_ctx_1);
    die2.GetDeclContextDIEs (decl_ctx_2);
    // Make sure the context arrays have the same size, otherwise
    // we are done
    const size_t count1 = decl_ctx_1.Size();
    const size_t count2 = decl_ctx_2.Size();
    if (count1 != count2)
        return false;
    
    // Make sure the DW_TAG values match all the way back up the
    // compile unit. If they don't, then we are done.
    DWARFDIE decl_ctx_die1;
    DWARFDIE decl_ctx_die2;
    size_t i;
    for (i=0; i<count1; i++)
    {
        decl_ctx_die1 = decl_ctx_1.GetDIEAtIndex (i);
        decl_ctx_die2 = decl_ctx_2.GetDIEAtIndex (i);
        if (decl_ctx_die1.Tag() != decl_ctx_die2.Tag())
            return false;
    }
#if defined LLDB_CONFIGURATION_DEBUG

    // Make sure the top item in the decl context die array is always 
    // DW_TAG_compile_unit. If it isn't then something went wrong in
    // the DWARFDIE::GetDeclContextDIEs() function...
    assert (decl_ctx_1.GetDIEAtIndex (count1 - 1).Tag() == DW_TAG_compile_unit);

#endif
    // Always skip the compile unit when comparing by only iterating up to
    // "count - 1". Here we compare the names as we go. 
    for (i=0; i<count1 - 1; i++)
    {
        decl_ctx_die1 = decl_ctx_1.GetDIEAtIndex (i);
        decl_ctx_die2 = decl_ctx_2.GetDIEAtIndex (i);
        const char *name1 = decl_ctx_die1.GetName();
        const char *name2 = decl_ctx_die2.GetName();
        // If the string was from a DW_FORM_strp, then the pointer will often
        // be the same!
        if (name1 == name2)
            continue;

        // Name pointers are not equal, so only compare the strings
        // if both are not NULL.
        if (name1 && name2)
        {
            // If the strings don't compare, we are done...
            if (strcmp(name1, name2) != 0)
                return false;
        }
        else
        {
            // One name was NULL while the other wasn't
            return false;
        }
    }
    // We made it through all of the checks and the declaration contexts
    // are equal.
    return true;
}
                                          

TypeSP
SymbolFileDWARF::FindDefinitionTypeForDWARFDeclContext (const DWARFDeclContext &dwarf_decl_ctx)
{
    TypeSP type_sp;

    const uint32_t dwarf_decl_ctx_count = dwarf_decl_ctx.GetSize();
    if (dwarf_decl_ctx_count > 0)
    {
        const ConstString type_name(dwarf_decl_ctx[0].name);
        const dw_tag_t tag = dwarf_decl_ctx[0].tag;

        if (type_name)
        {
            Log *log (LogChannelDWARF::GetLogIfAny(DWARF_LOG_TYPE_COMPLETION|DWARF_LOG_LOOKUPS));
            if (log)
            {
                GetObjectFile()->GetModule()->LogMessage (log,
                                                          "SymbolFileDWARF::FindDefinitionTypeForDWARFDeclContext(tag=%s, qualified-name='%s')",
                                                          DW_TAG_value_to_name(dwarf_decl_ctx[0].tag),
                                                          dwarf_decl_ctx.GetQualifiedName());
            }
            
            DIEArray die_offsets;
            
            if (m_using_apple_tables)
            {
                if (m_apple_types_ap.get())
                {
                    const bool has_tag = m_apple_types_ap->GetHeader().header_data.ContainsAtom (DWARFMappedHash::eAtomTypeTag);
                    const bool has_qualified_name_hash = m_apple_types_ap->GetHeader().header_data.ContainsAtom (DWARFMappedHash::eAtomTypeQualNameHash);
                    if (has_tag && has_qualified_name_hash)
                    {
                        const char *qualified_name = dwarf_decl_ctx.GetQualifiedName();
                        const uint32_t qualified_name_hash = MappedHash::HashStringUsingDJB (qualified_name);
                        if (log)
                            GetObjectFile()->GetModule()->LogMessage (log,"FindByNameAndTagAndQualifiedNameHash()");
                        m_apple_types_ap->FindByNameAndTagAndQualifiedNameHash (type_name.GetCString(), tag, qualified_name_hash, die_offsets);
                    }
                    else if (has_tag)
                    {
                        if (log)
                            GetObjectFile()->GetModule()->LogMessage (log,"FindByNameAndTag()");
                        m_apple_types_ap->FindByNameAndTag (type_name.GetCString(), tag, die_offsets);
                    }
                    else
                    {
                        m_apple_types_ap->FindByName (type_name.GetCString(), die_offsets);
                    }
                }
            }
            else
            {
                if (!m_indexed)
                    Index ();
                
                m_type_index.Find (type_name, die_offsets);
            }
            
            const size_t num_matches = die_offsets.size();
            
            
            if (num_matches)
            {
                DWARFDebugInfo* debug_info = DebugInfo();
                for (size_t i=0; i<num_matches; ++i)
                {
                    const DIERef& die_ref = die_offsets[i];
                    DWARFDIE type_die = debug_info->GetDIE (die_ref);
                    
                    if (type_die)
                    {
                        bool try_resolving_type = false;
                        
                        // Don't try and resolve the DIE we are looking for with the DIE itself!
                        const dw_tag_t type_tag = type_die.Tag();
                        // Make sure the tags match
                        if (type_tag == tag)
                        {
                            // The tags match, lets try resolving this type
                            try_resolving_type = true;
                        }
                        else
                        {
                            // The tags don't match, but we need to watch our for a
                            // forward declaration for a struct and ("struct foo")
                            // ends up being a class ("class foo { ... };") or
                            // vice versa.
                            switch (type_tag)
                            {
                                case DW_TAG_class_type:
                                    // We had a "class foo", see if we ended up with a "struct foo { ... };"
                                    try_resolving_type = (tag == DW_TAG_structure_type);
                                    break;
                                case DW_TAG_structure_type:
                                    // We had a "struct foo", see if we ended up with a "class foo { ... };"
                                    try_resolving_type = (tag == DW_TAG_class_type);
                                    break;
                                default:
                                    // Tags don't match, don't event try to resolve
                                    // using this type whose name matches....
                                    break;
                            }
                        }
                        
                        if (try_resolving_type)
                        {
                            DWARFDeclContext type_dwarf_decl_ctx;
                            type_die.GetDWARFDeclContext (type_dwarf_decl_ctx);

                            if (log)
                            {
                                GetObjectFile()->GetModule()->LogMessage (log,
                                                                          "SymbolFileDWARF::FindDefinitionTypeForDWARFDeclContext(tag=%s, qualified-name='%s') trying die=0x%8.8x (%s)",
                                                                          DW_TAG_value_to_name(dwarf_decl_ctx[0].tag),
                                                                          dwarf_decl_ctx.GetQualifiedName(),
                                                                          type_die.GetOffset(),
                                                                          type_dwarf_decl_ctx.GetQualifiedName());
                            }
                            
                            // Make sure the decl contexts match all the way up
                            if (dwarf_decl_ctx == type_dwarf_decl_ctx)
                            {
                                Type *resolved_type = ResolveType (type_die, false);
                                if (resolved_type && resolved_type != DIE_IS_BEING_PARSED)
                                {
                                    type_sp = resolved_type->shared_from_this();
                                    break;
                                }
                            }
                        }
                        else
                        {
                            if (log)
                            {
                                std::string qualified_name;
                                type_die.GetQualifiedName(qualified_name);
                                GetObjectFile()->GetModule()->LogMessage (log,
                                                                          "SymbolFileDWARF::FindDefinitionTypeForDWARFDeclContext(tag=%s, qualified-name='%s') ignoring die=0x%8.8x (%s)",
                                                                          DW_TAG_value_to_name(dwarf_decl_ctx[0].tag),
                                                                          dwarf_decl_ctx.GetQualifiedName(),
                                                                          type_die.GetOffset(),
                                                                          qualified_name.c_str());
                            }
                        }
                    }
                    else
                    {
                        if (m_using_apple_tables)
                        {
                            GetObjectFile()->GetModule()->ReportErrorIfModifyDetected ("the DWARF debug information has been modified (.apple_types accelerator table had bad die 0x%8.8x for '%s')\n",
                                                                                       die_ref.die_offset, type_name.GetCString());
                        }
                    }            
                    
                }
            }
        }
    }
    return type_sp;
}

TypeSP
SymbolFileDWARF::ParseType (const SymbolContext& sc, const DWARFDIE &die, bool *type_is_new_ptr)
{
    TypeSP type_sp;

    if (die)
    {
        TypeSystem *type_system = GetTypeSystemForLanguage(die.GetCU()->GetLanguageType());

        if (type_system)
        {
            DWARFASTParser *dwarf_ast = type_system->GetDWARFParser();
            if (dwarf_ast)
            {
                Log *log = LogChannelDWARF::GetLogIfAll(DWARF_LOG_DEBUG_INFO);
                type_sp = dwarf_ast->ParseTypeFromDWARF (sc, die, log, type_is_new_ptr);
                if (type_sp)
                {
                    TypeList* type_list = GetTypeList();
                    if (type_list)
                        type_list->Insert(type_sp);

                    if (die.Tag() == DW_TAG_subprogram)
                    {
                        DIERef die_ref = die.GetDIERef();
                        std::string scope_qualified_name(GetDeclContextForUID(die.GetID()).GetScopeQualifiedName().AsCString(""));
                        if (scope_qualified_name.size())
                        {
                            NameToOffsetMap::iterator iter = m_function_scope_qualified_name_map.find(scope_qualified_name);
                            if (iter != m_function_scope_qualified_name_map.end())
                                (*iter).second->insert(die_ref);
                            else
                            {
                                DIERefSetSP new_set(new std::set<DIERef>);
                                new_set->insert(die_ref);
                                m_function_scope_qualified_name_map.emplace(std::make_pair(scope_qualified_name, new_set));
                            }
                        }
                    }
                }
            }
        }
    }
    
    return type_sp;
}

size_t
SymbolFileDWARF::ParseTypes
(
    const SymbolContext& sc, 
    const DWARFDIE &orig_die,
    bool parse_siblings, 
    bool parse_children
)
{
    size_t types_added = 0;
    DWARFDIE die = orig_die;
    while (die)
    {
        bool type_is_new = false;
        if (ParseType(sc, die, &type_is_new).get())
        {
            if (type_is_new)
                ++types_added;
        }

        if (parse_children && die.HasChildren())
        {
            if (die.Tag() == DW_TAG_subprogram)
            {
                SymbolContext child_sc(sc);
                child_sc.function = sc.comp_unit->FindFunctionByUID(die.GetID()).get();
                types_added += ParseTypes(child_sc, die.GetFirstChild(), true, true);
            }
            else
                types_added += ParseTypes(sc, die.GetFirstChild(), true, true);
        }

        if (parse_siblings)
            die = die.GetSibling();
        else
            die.Clear();
    }
    return types_added;
}


size_t
SymbolFileDWARF::ParseFunctionBlocks (const SymbolContext &sc)
{
    assert(sc.comp_unit && sc.function);
    size_t functions_added = 0;
    DWARFCompileUnit* dwarf_cu = GetDWARFCompileUnit(sc.comp_unit);
    if (dwarf_cu)
    {
        const dw_offset_t function_die_offset = sc.function->GetID();
        DWARFDIE function_die = dwarf_cu->GetDIE (function_die_offset);
        if (function_die)
        {
            ParseFunctionBlocks(sc, &sc.function->GetBlock (false), function_die, LLDB_INVALID_ADDRESS, 0);
        }
    }

    return functions_added;
}


size_t
SymbolFileDWARF::ParseTypes (const SymbolContext &sc)
{
    // At least a compile unit must be valid
    assert(sc.comp_unit);
    size_t types_added = 0;
    DWARFCompileUnit* dwarf_cu = GetDWARFCompileUnit(sc.comp_unit);
    if (dwarf_cu)
    {
        if (sc.function)
        {
            dw_offset_t function_die_offset = sc.function->GetID();
            DWARFDIE func_die = dwarf_cu->GetDIE(function_die_offset);
            if (func_die && func_die.HasChildren())
            {
                types_added = ParseTypes(sc, func_die.GetFirstChild(), true, true);
            }
        }
        else
        {
            DWARFDIE dwarf_cu_die = dwarf_cu->DIE();
            if (dwarf_cu_die && dwarf_cu_die.HasChildren())
            {
                types_added = ParseTypes(sc, dwarf_cu_die.GetFirstChild(), true, true);
            }
        }
    }

    return types_added;
}

size_t
SymbolFileDWARF::ParseVariablesForContext (const SymbolContext& sc)
{
    if (sc.comp_unit != NULL)
    {
        DWARFDebugInfo* info = DebugInfo();
        if (info == NULL)
            return 0;
        
        if (sc.function)
        {
            DWARFDIE function_die = info->GetDIE(DIERef(sc.function->GetID()));
            
            const dw_addr_t func_lo_pc = function_die.GetAttributeValueAsAddress (DW_AT_low_pc, LLDB_INVALID_ADDRESS);
            if (func_lo_pc != LLDB_INVALID_ADDRESS)
            {
                const size_t num_variables = ParseVariables(sc, function_die.GetFirstChild(), func_lo_pc, true, true);
            
                // Let all blocks know they have parse all their variables
                sc.function->GetBlock (false).SetDidParseVariables (true, true);
                return num_variables;
            }
        }
        else if (sc.comp_unit)
        {
            DWARFCompileUnit* dwarf_cu = info->GetCompileUnit(sc.comp_unit->GetID());

            if (dwarf_cu == NULL)
                return 0;

            uint32_t vars_added = 0;
            VariableListSP variables (sc.comp_unit->GetVariableList(false));
            
            if (variables.get() == NULL)
            {
                variables.reset(new VariableList());
                sc.comp_unit->SetVariableList(variables);

                DIEArray die_offsets;
                if (m_using_apple_tables)
                {
                    if (m_apple_names_ap.get())
                    {
                        DWARFMappedHash::DIEInfoArray hash_data_array;
                        if (m_apple_names_ap->AppendAllDIEsInRange (dwarf_cu->GetOffset(), 
                                                                    dwarf_cu->GetNextCompileUnitOffset(), 
                                                                    hash_data_array))
                        {
                            DWARFMappedHash::ExtractDIEArray (hash_data_array, die_offsets);
                        }
                    }
                }
                else
                {
                    // Index if we already haven't to make sure the compile units
                    // get indexed and make their global DIE index list
                    if (!m_indexed)
                        Index ();

                    m_global_index.FindAllEntriesForCompileUnit (dwarf_cu->GetOffset(), 
                                                                 die_offsets);
                }

                const size_t num_matches = die_offsets.size();
                if (num_matches)
                {
                    DWARFDebugInfo* debug_info = DebugInfo();
                    for (size_t i=0; i<num_matches; ++i)
                    {
                        const DIERef& die_ref = die_offsets[i];
                        DWARFDIE die = debug_info->GetDIE (die_ref);
                        if (die)
                        {
                            VariableSP var_sp (ParseVariableDIE(sc, die, LLDB_INVALID_ADDRESS));
                            if (var_sp)
                            {
                                variables->AddVariableIfUnique (var_sp);
                                ++vars_added;
                            }
                        }
                        else
                        {
                            if (m_using_apple_tables)
                            {
                                GetObjectFile()->GetModule()->ReportErrorIfModifyDetected ("the DWARF debug information has been modified (.apple_names accelerator table had bad die 0x%8.8x)\n", die_ref.die_offset);
                            }
                        }            

                    }
                }
            }
            return vars_added;
        }
    }
    return 0;
}

VariableSP
SymbolFileDWARF::ParseVariableDIE
(
    const SymbolContext& sc,
    const DWARFDIE &die,
    const lldb::addr_t func_low_pc
)
{
    if (die.GetDWARF() != this)
        return die.GetDWARF()->ParseVariableDIE(sc, die, func_low_pc);

    VariableSP var_sp;
    if (!die)
        return var_sp;

    var_sp = GetDIEToVariable()[die.GetDIE()];
    if (var_sp)
        return var_sp;  // Already been parsed!
    
    const dw_tag_t tag = die.Tag();
    ModuleSP module = GetObjectFile()->GetModule();
    
    if ((tag == DW_TAG_variable) ||
        (tag == DW_TAG_constant) ||
        (tag == DW_TAG_formal_parameter && sc.function))
    {
        DWARFAttributes attributes;
        const size_t num_attributes = die.GetAttributes(attributes);
        DWARFDIE spec_die;
        if (num_attributes > 0)
        {
            const char *name = NULL;
            const char *mangled = NULL;
            Declaration decl;
            uint32_t i;
            DWARFFormValue type_die_form;
            DWARFExpression location(die.GetCU());
            bool is_external = false;
            bool is_artificial = false;
            bool location_is_const_value_data = false;
            bool has_explicit_location = false;
            DWARFFormValue const_value;
            Variable::RangeList scope_ranges;
            //AccessType accessibility = eAccessNone;

            for (i=0; i<num_attributes; ++i)
            {
                dw_attr_t attr = attributes.AttributeAtIndex(i);
                DWARFFormValue form_value;
                
                if (attributes.ExtractFormValueAtIndex(i, form_value))
                {
                    switch (attr)
                    {
                    case DW_AT_decl_file:   decl.SetFile(sc.comp_unit->GetSupportFiles().GetFileSpecAtIndex(form_value.Unsigned())); break;
                    case DW_AT_decl_line:   decl.SetLine(form_value.Unsigned()); break;
                    case DW_AT_decl_column: decl.SetColumn(form_value.Unsigned()); break;
                    case DW_AT_name:        name = form_value.AsCString(); break;
                    case DW_AT_linkage_name:
                    case DW_AT_MIPS_linkage_name: mangled = form_value.AsCString(); break;
                    case DW_AT_type:        type_die_form = form_value; break;
                    case DW_AT_external:    is_external = form_value.Boolean(); break;
                    case DW_AT_const_value:
                        // If we have already found a DW_AT_location attribute, ignore this attribute.
                        if (!has_explicit_location)
                        {
                            location_is_const_value_data = true;
                            // The constant value will be either a block, a data value or a string.
                            const DWARFDataExtractor& debug_info_data = get_debug_info_data();
                            if (DWARFFormValue::IsBlockForm(form_value.Form()))
                            {
                                // Retrieve the value as a block expression.
                                uint32_t block_offset = form_value.BlockData() - debug_info_data.GetDataStart();
                                uint32_t block_length = form_value.Unsigned();
                                location.CopyOpcodeData(module, debug_info_data, block_offset, block_length);
                            }
                            else if (DWARFFormValue::IsDataForm(form_value.Form()))
                            {
                                // Retrieve the value as a data expression.
                                DWARFFormValue::FixedFormSizes fixed_form_sizes =
                                    DWARFFormValue::GetFixedFormSizesForAddressSize (
                                            attributes.CompileUnitAtIndex(i)->GetAddressByteSize(),
                                            attributes.CompileUnitAtIndex(i)->IsDWARF64());
                                uint32_t data_offset = attributes.DIEOffsetAtIndex(i);
                                uint32_t data_length = fixed_form_sizes.GetSize(form_value.Form());
                                if (data_length == 0)
                                {
                                    const uint8_t *data_pointer = form_value.BlockData();
                                    if (data_pointer)
                                    {
                                        form_value.Unsigned();
                                    }
                                    else if (DWARFFormValue::IsDataForm(form_value.Form()))
                                    {
                                        // we need to get the byte size of the type later after we create the variable
                                        const_value = form_value;
                                    }
                                }
                                else
                                    location.CopyOpcodeData(module, debug_info_data, data_offset, data_length);
                            }
                            else
                            {
                                // Retrieve the value as a string expression.
                                if (form_value.Form() == DW_FORM_strp)
                                {
                                    DWARFFormValue::FixedFormSizes fixed_form_sizes =
                                        DWARFFormValue::GetFixedFormSizesForAddressSize (
                                                attributes.CompileUnitAtIndex(i)->GetAddressByteSize(),
                                                attributes.CompileUnitAtIndex(i)->IsDWARF64());
                                    uint32_t data_offset = attributes.DIEOffsetAtIndex(i);
                                    uint32_t data_length = fixed_form_sizes.GetSize(form_value.Form());
                                    location.CopyOpcodeData(module, debug_info_data, data_offset, data_length);
                                }
                                else
                                {
                                    const char *str = form_value.AsCString();
                                    uint32_t string_offset = str - (const char *)debug_info_data.GetDataStart();
                                    uint32_t string_length = strlen(str) + 1;
                                    location.CopyOpcodeData(module, debug_info_data, string_offset, string_length);
                                }
                            }
                        }
                        break;
                    case DW_AT_location:
                        {
                            location_is_const_value_data = false;
                            has_explicit_location = true;
                            if (form_value.BlockData())
                            {
                                const DWARFDataExtractor& debug_info_data = get_debug_info_data();

                                uint32_t block_offset = form_value.BlockData() - debug_info_data.GetDataStart();
                                uint32_t block_length = form_value.Unsigned();
                                location.CopyOpcodeData(module, get_debug_info_data(), block_offset, block_length);
                            }
                            else
                            {
                                const DWARFDataExtractor& debug_loc_data = get_debug_loc_data();
                                const dw_offset_t debug_loc_offset = form_value.Unsigned();

                                size_t loc_list_length = DWARFExpression::LocationListSize(die.GetCU(), debug_loc_data, debug_loc_offset);
                                if (loc_list_length > 0)
                                {
                                    location.CopyOpcodeData(module, debug_loc_data, debug_loc_offset, loc_list_length);
                                    assert (func_low_pc != LLDB_INVALID_ADDRESS);
                                    location.SetLocationListSlide (func_low_pc - attributes.CompileUnitAtIndex(i)->GetBaseAddress());
                                }
                            }
                        }
                        break;
                    case DW_AT_specification:
                    {
                        DWARFDebugInfo* debug_info = DebugInfo();
                        if (debug_info)
                            spec_die = debug_info->GetDIE(DIERef(form_value));
                        break;
                    }
                    case DW_AT_start_scope:
                    {
                        if (form_value.Form() == DW_FORM_sec_offset)
                        {
                            DWARFRangeList dwarf_scope_ranges;
                            const DWARFDebugRanges* debug_ranges = DebugRanges();
                            debug_ranges->FindRanges(form_value.Unsigned(), dwarf_scope_ranges);

                            // All DW_AT_start_scope are relative to the base address of the
                            // compile unit. We add the compile unit base address to make
                            // sure all the addresses are properly fixed up.
                            for (size_t i = 0, count = dwarf_scope_ranges.GetSize(); i < count; ++i)
                            {
                                const DWARFRangeList::Entry& range = dwarf_scope_ranges.GetEntryRef(i);
                                scope_ranges.Append(range.GetRangeBase() + die.GetCU()->GetBaseAddress(),
                                                    range.GetByteSize());
                            }
                        }
                        else
                        {
                            // TODO: Handle the case when DW_AT_start_scope have form constant. The 
                            // dwarf spec is a bit ambiguous about what is the expected behavior in
                            // case the enclosing block have a non coninious address range and the
                            // DW_AT_start_scope entry have a form constant. 
                            GetObjectFile()->GetModule()->ReportWarning ("0x%8.8" PRIx64 ": DW_AT_start_scope has unsupported form type (0x%x)\n",
                                                                         die.GetID(),
                                                                         form_value.Form());
                        }

                        scope_ranges.Sort();
                        scope_ranges.CombineConsecutiveRanges();
                    }
                    case DW_AT_artificial:      is_artificial = form_value.Boolean(); break;
                    case DW_AT_accessibility:   break; //accessibility = DW_ACCESS_to_AccessType(form_value.Unsigned()); break;
                    case DW_AT_declaration:
                    case DW_AT_description:
                    case DW_AT_endianity:
                    case DW_AT_segment:
                    case DW_AT_visibility:
                    default:
                    case DW_AT_abstract_origin:
                    case DW_AT_sibling:
                        break;
                    }
                }
            }

            const DWARFDIE parent_context_die = GetDeclContextDIEContainingDIE(die);
            const dw_tag_t parent_tag = die.GetParent().Tag();
            bool is_static_member = parent_tag == DW_TAG_compile_unit && (parent_context_die.Tag() == DW_TAG_class_type || parent_context_die.Tag() == DW_TAG_structure_type);

            ValueType scope = eValueTypeInvalid;

            const DWARFDIE sc_parent_die = GetParentSymbolContextDIE(die);
            SymbolContextScope * symbol_context_scope = NULL;

            if (!mangled)
            {
                // LLDB relies on the mangled name (DW_TAG_linkage_name or DW_AT_MIPS_linkage_name) to
                // generate fully qualified names of global variables with commands like "frame var j".
                // For example, if j were an int variable holding a value 4 and declared in a namespace
                // B which in turn is contained in a namespace A, the command "frame var j" returns
                // "(int) A::B::j = 4". If the compiler does not emit a linkage name, we should be able
                // to generate a fully qualified name from the declaration context.
                if (parent_tag == DW_TAG_compile_unit &&
                    Language::LanguageIsCPlusPlus(die.GetLanguage()))
                {
                    DWARFDeclContext decl_ctx;

                    die.GetDWARFDeclContext(decl_ctx);
                    mangled = decl_ctx.GetQualifiedNameAsConstString().GetCString();
                }
            }

            // DWARF doesn't specify if a DW_TAG_variable is a local, global
            // or static variable, so we have to do a little digging by
            // looking at the location of a variable to see if it contains
            // a DW_OP_addr opcode _somewhere_ in the definition. I say
            // somewhere because clang likes to combine small global variables
            // into the same symbol and have locations like:
            // DW_OP_addr(0x1000), DW_OP_constu(2), DW_OP_plus
            // So if we don't have a DW_TAG_formal_parameter, we can look at
            // the location to see if it contains a DW_OP_addr opcode, and
            // then we can correctly classify  our variables.
            if (tag == DW_TAG_formal_parameter)
                scope = eValueTypeVariableArgument;
            else
            {
                bool op_error = false;
                // Check if the location has a DW_OP_addr with any address value...
                lldb::addr_t location_DW_OP_addr = LLDB_INVALID_ADDRESS;
                if (!location_is_const_value_data)
                {
                    location_DW_OP_addr = location.GetLocation_DW_OP_addr (0, op_error);
                    if (op_error)
                    {
                        StreamString strm;
                        location.DumpLocationForAddress (&strm, eDescriptionLevelFull, 0, 0, NULL);
                        GetObjectFile()->GetModule()->ReportError ("0x%8.8x: %s has an invalid location: %s", die.GetOffset(), die.GetTagAsCString(), strm.GetString().c_str());
                    }
                }

                if (location_DW_OP_addr != LLDB_INVALID_ADDRESS)
                {
                    if (is_external)
                        scope = eValueTypeVariableGlobal;
                    else
                        scope = eValueTypeVariableStatic;
                    
                    
                    SymbolFileDWARFDebugMap *debug_map_symfile = GetDebugMapSymfile ();
                    
                    if (debug_map_symfile)
                    {
                        // When leaving the DWARF in the .o files on darwin,
                        // when we have a global variable that wasn't initialized,
                        // the .o file might not have allocated a virtual
                        // address for the global variable. In this case it will
                        // have created a symbol for the global variable
                        // that is undefined/data and external and the value will
                        // be the byte size of the variable. When we do the
                        // address map in SymbolFileDWARFDebugMap we rely on
                        // having an address, we need to do some magic here
                        // so we can get the correct address for our global
                        // variable. The address for all of these entries
                        // will be zero, and there will be an undefined symbol
                        // in this object file, and the executable will have
                        // a matching symbol with a good address. So here we
                        // dig up the correct address and replace it in the
                        // location for the variable, and set the variable's
                        // symbol context scope to be that of the main executable
                        // so the file address will resolve correctly.
                        bool linked_oso_file_addr = false;
                        if (is_external && location_DW_OP_addr == 0)
                        {
                            // we have a possible uninitialized extern global
                            ConstString const_name(mangled ? mangled : name);
                            ObjectFile *debug_map_objfile = debug_map_symfile->GetObjectFile();
                            if (debug_map_objfile)
                            {
                                Symtab *debug_map_symtab = debug_map_objfile->GetSymtab();
                                if (debug_map_symtab)
                                {
                                    Symbol *exe_symbol = debug_map_symtab->FindFirstSymbolWithNameAndType (const_name,
                                                                                                           eSymbolTypeData,
                                                                                                           Symtab::eDebugYes,
                                                                                                           Symtab::eVisibilityExtern);
                                    if (exe_symbol)
                                    {
                                        if (exe_symbol->ValueIsAddress())
                                        {
                                            const addr_t exe_file_addr = exe_symbol->GetAddressRef().GetFileAddress();
                                            if (exe_file_addr != LLDB_INVALID_ADDRESS)
                                            {
                                                if (location.Update_DW_OP_addr (exe_file_addr))
                                                {
                                                    linked_oso_file_addr = true;
                                                    symbol_context_scope = exe_symbol;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        if (!linked_oso_file_addr)
                        {
                            // The DW_OP_addr is not zero, but it contains a .o file address which
                            // needs to be linked up correctly.
                            const lldb::addr_t exe_file_addr = debug_map_symfile->LinkOSOFileAddress(this, location_DW_OP_addr);
                            if (exe_file_addr != LLDB_INVALID_ADDRESS)
                            {
                                // Update the file address for this variable
                                location.Update_DW_OP_addr (exe_file_addr);
                            }
                            else
                            {
                                // Variable didn't make it into the final executable
                                return var_sp;
                            }
                        }
                    }
                }
                else
                {
                    if (location_is_const_value_data)
                        scope = eValueTypeVariableStatic;
                    else
                        scope = eValueTypeVariableLocal;
                }
            }

            if (symbol_context_scope == NULL)
            {
                switch (parent_tag)
                {
                case DW_TAG_subprogram:
                case DW_TAG_inlined_subroutine:
                case DW_TAG_lexical_block:
                    if (sc.function)
                    {
                        symbol_context_scope = sc.function->GetBlock(true).FindBlockByID(sc_parent_die.GetID());
                        if (symbol_context_scope == NULL)
                            symbol_context_scope = sc.function;
                    }
                    break;
                
                default:
                    symbol_context_scope = sc.comp_unit;
                    break;
                }
            }

            if (symbol_context_scope)
            {
                SymbolFileTypeSP type_sp(new SymbolFileType(*this, DIERef(type_die_form).GetUID()));

                if (const_value.Form() && type_sp && type_sp->GetType())
                    location.CopyOpcodeData(const_value.Unsigned(), type_sp->GetType()->GetByteSize(), die.GetCU()->GetAddressByteSize());

                var_sp.reset (new Variable (die.GetID(),
                                            name,
                                            mangled,
                                            type_sp,
                                            scope,
                                            symbol_context_scope,
                                            scope_ranges,
                                            &decl,
                                            location,
                                            is_external,
                                            is_artificial,
                                            is_static_member));
                
                var_sp->SetLocationIsConstantValueData (location_is_const_value_data);
            }
            else
            {
                // Not ready to parse this variable yet. It might be a global
                // or static variable that is in a function scope and the function
                // in the symbol context wasn't filled in yet
                return var_sp;
            }
        }
        // Cache var_sp even if NULL (the variable was just a specification or
        // was missing vital information to be able to be displayed in the debugger
        // (missing location due to optimization, etc)) so we don't re-parse
        // this DIE over and over later...
        GetDIEToVariable()[die.GetDIE()] = var_sp;
        if (spec_die)
            GetDIEToVariable()[spec_die.GetDIE()] = var_sp;
    }
    return var_sp;
}


DWARFDIE
SymbolFileDWARF::FindBlockContainingSpecification (const DIERef& func_die_ref,
                                                   dw_offset_t spec_block_die_offset)
{
    // Give the concrete function die specified by "func_die_offset", find the 
    // concrete block whose DW_AT_specification or DW_AT_abstract_origin points
    // to "spec_block_die_offset"
    return FindBlockContainingSpecification (DebugInfo()->GetDIE (func_die_ref), spec_block_die_offset);
}


DWARFDIE
SymbolFileDWARF::FindBlockContainingSpecification(const DWARFDIE &die,
                                                  dw_offset_t spec_block_die_offset)
{
    if (die)
    {
        switch (die.Tag())
        {
        case DW_TAG_subprogram:
        case DW_TAG_inlined_subroutine:
        case DW_TAG_lexical_block:
            {
                if (die.GetAttributeValueAsReference (DW_AT_specification, DW_INVALID_OFFSET) == spec_block_die_offset)
                    return die;

                if (die.GetAttributeValueAsReference (DW_AT_abstract_origin, DW_INVALID_OFFSET) == spec_block_die_offset)
                    return die;
            }
            break;
        }

        // Give the concrete function die specified by "func_die_offset", find the 
        // concrete block whose DW_AT_specification or DW_AT_abstract_origin points
        // to "spec_block_die_offset"
        for (DWARFDIE child_die = die.GetFirstChild(); child_die; child_die = child_die.GetSibling())
        {
            DWARFDIE result_die = FindBlockContainingSpecification (child_die, spec_block_die_offset);
            if (result_die)
                return result_die;
        }
    }
    
    return DWARFDIE();
}

size_t
SymbolFileDWARF::ParseVariables (const SymbolContext& sc,
                                 const DWARFDIE &orig_die,
                                 const lldb::addr_t func_low_pc,
                                 bool parse_siblings,
                                 bool parse_children,
                                 VariableList* cc_variable_list)
{
    if (!orig_die)
        return 0;

    VariableListSP variable_list_sp;

    size_t vars_added = 0;
    DWARFDIE die = orig_die;
    while (die)
    {
        dw_tag_t tag = die.Tag();

        // Check to see if we have already parsed this variable or constant?
        VariableSP var_sp = GetDIEToVariable()[die.GetDIE()];
        if (var_sp)
        {
            if (cc_variable_list)
                cc_variable_list->AddVariableIfUnique (var_sp);
        }
        else
        {
            // We haven't already parsed it, lets do that now.
            if ((tag == DW_TAG_variable) ||
                (tag == DW_TAG_constant) ||
                (tag == DW_TAG_formal_parameter && sc.function))
            {
                if (variable_list_sp.get() == NULL)
                {
                    DWARFDIE sc_parent_die = GetParentSymbolContextDIE(orig_die);
                    dw_tag_t parent_tag = sc_parent_die.Tag();
                    switch (parent_tag)
                    {
                        case DW_TAG_compile_unit:
                            if (sc.comp_unit != NULL)
                            {
                                variable_list_sp = sc.comp_unit->GetVariableList(false);
                                if (variable_list_sp.get() == NULL)
                                {
                                    variable_list_sp.reset(new VariableList());
                                    sc.comp_unit->SetVariableList(variable_list_sp);
                                }
                            }
                            else
                            {
                                GetObjectFile()->GetModule()->ReportError ("parent 0x%8.8" PRIx64 " %s with no valid compile unit in symbol context for 0x%8.8" PRIx64 " %s.\n",
                                                                           sc_parent_die.GetID(),
                                                                           sc_parent_die.GetTagAsCString(),
                                                                           orig_die.GetID(),
                                                                           orig_die.GetTagAsCString());
                            }
                            break;
                            
                        case DW_TAG_subprogram:
                        case DW_TAG_inlined_subroutine:
                        case DW_TAG_lexical_block:
                            if (sc.function != NULL)
                            {
                                // Check to see if we already have parsed the variables for the given scope
                                
                                Block *block = sc.function->GetBlock(true).FindBlockByID(sc_parent_die.GetID());
                                if (block == NULL)
                                {
                                    // This must be a specification or abstract origin with 
                                    // a concrete block counterpart in the current function. We need
                                    // to find the concrete block so we can correctly add the 
                                    // variable to it
                                    const DWARFDIE concrete_block_die = FindBlockContainingSpecification (DIERef(sc.function->GetID()),
                                                                                                          sc_parent_die.GetOffset());
                                    if (concrete_block_die)
                                        block = sc.function->GetBlock(true).FindBlockByID(concrete_block_die.GetID());
                                }
                                
                                if (block != NULL)
                                {
                                    const bool can_create = false;
                                    variable_list_sp = block->GetBlockVariableList (can_create);
                                    if (variable_list_sp.get() == NULL)
                                    {
                                        variable_list_sp.reset(new VariableList());
                                        block->SetVariableList(variable_list_sp);
                                    }
                                }
                            }
                            break;
                            
                        default:
                             GetObjectFile()->GetModule()->ReportError ("didn't find appropriate parent DIE for variable list for 0x%8.8" PRIx64 " %s.\n",
                                                                        orig_die.GetID(),
                                                                        orig_die.GetTagAsCString());
                            break;
                    }
                }
                
                if (variable_list_sp)
                {
                    VariableSP var_sp (ParseVariableDIE(sc, die, func_low_pc));
                    if (var_sp)
                    {
                        variable_list_sp->AddVariableIfUnique (var_sp);
                        if (cc_variable_list)
                            cc_variable_list->AddVariableIfUnique (var_sp);
                        ++vars_added;
                    }
                }
            }
        }

        bool skip_children = (sc.function == NULL && tag == DW_TAG_subprogram);

        if (!skip_children && parse_children && die.HasChildren())
        {
            vars_added += ParseVariables(sc, die.GetFirstChild(), func_low_pc, true, true, cc_variable_list);
        }

        if (parse_siblings)
            die = die.GetSibling();
        else
            die.Clear();
    }
    return vars_added;
}

//------------------------------------------------------------------
// PluginInterface protocol
//------------------------------------------------------------------
ConstString
SymbolFileDWARF::GetPluginName()
{
    return GetPluginNameStatic();
}

uint32_t
SymbolFileDWARF::GetPluginVersion()
{
    return 1;
}

void
SymbolFileDWARF::DumpIndexes ()
{
    StreamFile s(stdout, false);
    
    s.Printf ("DWARF index for (%s) '%s':",
              GetObjectFile()->GetModule()->GetArchitecture().GetArchitectureName(),
              GetObjectFile()->GetFileSpec().GetPath().c_str());
    s.Printf("\nFunction basenames:\n");    m_function_basename_index.Dump (&s);
    s.Printf("\nFunction fullnames:\n");    m_function_fullname_index.Dump (&s);
    s.Printf("\nFunction methods:\n");      m_function_method_index.Dump (&s);
    s.Printf("\nFunction selectors:\n");    m_function_selector_index.Dump (&s);
    s.Printf("\nObjective C class selectors:\n");    m_objc_class_selectors_index.Dump (&s);
    s.Printf("\nGlobals and statics:\n");   m_global_index.Dump (&s); 
    s.Printf("\nTypes:\n");                 m_type_index.Dump (&s);
    s.Printf("\nNamespaces:\n");            m_namespace_index.Dump (&s);
}


SymbolFileDWARFDebugMap *
SymbolFileDWARF::GetDebugMapSymfile ()
{
    if (m_debug_map_symfile == NULL && !m_debug_map_module_wp.expired())
    {
        lldb::ModuleSP module_sp (m_debug_map_module_wp.lock());
        if (module_sp)
        {
            SymbolVendor *sym_vendor = module_sp->GetSymbolVendor();
            if (sym_vendor)
                m_debug_map_symfile = (SymbolFileDWARFDebugMap *)sym_vendor->GetSymbolFile();
        }
    }
    return m_debug_map_symfile;
}

DWARFExpression::LocationListFormat
SymbolFileDWARF::GetLocationListFormat() const
{
    return DWARFExpression::RegularLocationList;
}
