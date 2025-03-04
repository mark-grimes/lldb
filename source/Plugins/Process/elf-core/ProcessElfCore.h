//===-- ProcessElfCore.h ----------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// Notes about Linux Process core dumps:
//  1) Linux core dump is stored as ELF file.
//  2) The ELF file's PT_NOTE and PT_LOAD segments describes the program's
//     address space and thread contexts.
//  3) PT_NOTE segment contains note entries which describes a thread context.
//  4) PT_LOAD segment describes a valid contiguous range of process address
//     space.
//===----------------------------------------------------------------------===//

#ifndef liblldb_ProcessElfCore_h_
#define liblldb_ProcessElfCore_h_

// C Includes
// C++ Includes
#include <list>
#include <vector>

// Other libraries and framework includes
// Project includes
#include "lldb/Core/ConstString.h"
#include "lldb/Core/Error.h"
#include "lldb/Target/Process.h"

#include "Plugins/ObjectFile/ELF/ELFHeader.h"

struct ThreadData;

class ProcessElfCore : public lldb_private::Process
{
public:
    //------------------------------------------------------------------
    // Constructors and Destructors
    //------------------------------------------------------------------
    static lldb::ProcessSP
    CreateInstance (lldb::TargetSP target_sp,
                    lldb::ListenerSP listener_sp,
                    const lldb_private::FileSpec *crash_file_path);

    static void
    Initialize();

    static void
    Terminate();

    static lldb_private::ConstString
    GetPluginNameStatic();

    static const char *
    GetPluginDescriptionStatic();

    //------------------------------------------------------------------
    // Constructors and Destructors
    //------------------------------------------------------------------
    ProcessElfCore(lldb::TargetSP target_sp,
                   lldb::ListenerSP listener_sp,
                   const lldb_private::FileSpec &core_file);

    ~ProcessElfCore() override;

    //------------------------------------------------------------------
    // Check if a given Process
    //------------------------------------------------------------------
    bool CanDebug(lldb::TargetSP target_sp, bool plugin_specified_by_name) override;

    //------------------------------------------------------------------
    // Creating a new process, or attaching to an existing one
    //------------------------------------------------------------------
    lldb_private::Error DoLoadCore() override;

    lldb_private::DynamicLoader *GetDynamicLoader() override;

    //------------------------------------------------------------------
    // PluginInterface protocol
    //------------------------------------------------------------------
    lldb_private::ConstString GetPluginName() override;

    uint32_t GetPluginVersion() override;

    //------------------------------------------------------------------
    // Process Control
    //------------------------------------------------------------------
    lldb_private::Error DoDestroy() override;

    void RefreshStateAfterStop() override;

    //------------------------------------------------------------------
    // Process Queries
    //------------------------------------------------------------------
    bool IsAlive() override;

    //------------------------------------------------------------------
    // Process Memory
    //------------------------------------------------------------------
    size_t ReadMemory(lldb::addr_t addr, void *buf, size_t size, lldb_private::Error &error) override;

    size_t DoReadMemory(lldb::addr_t addr, void *buf, size_t size, lldb_private::Error &error) override;

    lldb::addr_t GetImageInfoAddress() override;

    lldb_private::ArchSpec
    GetArchitecture();

    // Returns AUXV structure found in the core file
    const lldb::DataBufferSP
    GetAuxvData() override;

protected:
    void
    Clear ( );

    bool UpdateThreadList(lldb_private::ThreadList &old_thread_list,
                          lldb_private::ThreadList &new_thread_list) override;

private:
    struct NT_FILE_Entry
    {
        lldb::addr_t start;
        lldb::addr_t end;
        lldb::addr_t file_ofs;
        lldb_private::ConstString path;
    };

    //------------------------------------------------------------------
    // For ProcessElfCore only
    //------------------------------------------------------------------
    typedef lldb_private::Range<lldb::addr_t, lldb::addr_t> FileRange;
    typedef lldb_private::RangeDataArray<lldb::addr_t, lldb::addr_t, FileRange, 1> VMRangeToFileOffset;

    lldb::ModuleSP m_core_module_sp;
    lldb_private::FileSpec m_core_file;
    std::string  m_dyld_plugin_name;
    DISALLOW_COPY_AND_ASSIGN (ProcessElfCore);

    llvm::Triple::OSType m_os;

    // True if m_thread_contexts contains valid entries
    bool m_thread_data_valid;

    // Contain thread data read from NOTE segments
    std::vector<ThreadData> m_thread_data;

    // AUXV structure found from the NOTE segment
    lldb_private::DataExtractor m_auxv;

    // Address ranges found in the core
    VMRangeToFileOffset m_core_aranges;

    // NT_FILE entries found from the NOTE segment
    std::vector<NT_FILE_Entry> m_nt_file_entries;

    // Parse thread(s) data structures(prstatus, prpsinfo) from given NOTE segment
    void
    ParseThreadContextsFromNoteSegment (const elf::ELFProgramHeader *segment_header,
                                        lldb_private::DataExtractor segment_data);

    // Returns number of thread contexts stored in the core file
    uint32_t
    GetNumThreadContexts();

    // Parse a contiguous address range of the process from LOAD segment
    lldb::addr_t
    AddAddressRangeFromLoadSegment(const elf::ELFProgramHeader *header);
};

#endif // liblldb_ProcessElfCore_h_
