//===-- IRExecutionUnit.cpp -------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include "lldb/Core/DataBufferHeap.h"
#include "lldb/Core/DataExtractor.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/Disassembler.h"
#include "lldb/Core/Log.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/Section.h"
#include "llvm/ExecutionEngine/MCJIT.h"
#include "lldb/Symbol/CompileUnit.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Symbol/SymbolVendor.h"
#include "lldb/Symbol/SymbolFile.h"
#include "lldb/Expression/IRExecutionUnit.h"
#include "lldb/Symbol/Symbol.h"
#include "lldb/Symbol/SymbolVendor.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/ObjCLanguageRuntime.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/ObjCLanguageRuntime.h"

#include "lldb/../../source/Plugins/Language/CPlusPlus/CPlusPlusLanguage.h"

using namespace lldb_private;

IRExecutionUnit::IRExecutionUnit (std::unique_ptr<llvm::LLVMContext> &context_ap,
                                  std::unique_ptr<llvm::Module> &module_ap,
                                  ConstString &name,
                                  const lldb::TargetSP &target_sp,
                                  const SymbolContext &sym_ctx,
                                  std::vector<std::string> &cpu_features) :
    IRMemoryMap(target_sp),
    m_context_ap(context_ap.release()),
    m_module_ap(module_ap.release()),
    m_jit_module_wp(),
    m_module(m_module_ap.get()),
    m_cpu_features(cpu_features),
    m_name(name),
    m_sym_ctx(sym_ctx),
    m_did_jit(false),
    m_function_load_addr(LLDB_INVALID_ADDRESS),
    m_function_end_load_addr(LLDB_INVALID_ADDRESS)
{
}

IRExecutionUnit::~IRExecutionUnit ()
{
    Mutex::Locker global_context_locker(IRExecutionUnit::GetLLVMGlobalContextMutex());
    
    m_module_ap.reset();
    m_execution_engine_ap.reset();
    m_context_ap.reset();
    
    lldb::ModuleSP jit_module_sp (m_jit_module_wp.lock());
    if (jit_module_sp)
    {
        ExecutionContext exe_ctx (GetBestExecutionContextScope());
        Target *target = exe_ctx.GetTargetPtr();
        if (target)
            target->GetImages().Remove(jit_module_sp);
    }
}


lldb::addr_t
IRExecutionUnit::WriteNow (const uint8_t *bytes,
                           size_t size,
                           Error &error)
{
    const bool zero_memory = false;
    lldb::addr_t allocation_process_addr = Malloc (size,
                                                   8,
                                                   lldb::ePermissionsWritable | lldb::ePermissionsReadable,
                                                   eAllocationPolicyMirror,
                                                   zero_memory,
                                                   error);

    if (!error.Success())
        return LLDB_INVALID_ADDRESS;

    WriteMemory(allocation_process_addr, bytes, size, error);

    if (!error.Success())
    {
        Error err;
        Free (allocation_process_addr, err);

        return LLDB_INVALID_ADDRESS;
    }

    if (Log *log = lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS))
    {
        DataBufferHeap my_buffer(size, 0);
        Error err;
        ReadMemory(my_buffer.GetBytes(), allocation_process_addr, size, err);

        if (err.Success())
        {
            DataExtractor my_extractor(my_buffer.GetBytes(), my_buffer.GetByteSize(), lldb::eByteOrderBig, 8);
            my_extractor.PutToLog(log, 0, my_buffer.GetByteSize(), allocation_process_addr, 16, DataExtractor::TypeUInt8);
        }
    }

    return allocation_process_addr;
}

void
IRExecutionUnit::FreeNow (lldb::addr_t allocation)
{
    if (allocation == LLDB_INVALID_ADDRESS)
        return;

    Error err;

    Free(allocation, err);
}

Error
IRExecutionUnit::DisassembleFunction (Stream &stream,
                                      lldb::ProcessSP &process_wp)
{
    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));

    ExecutionContext exe_ctx(process_wp);

    Error ret;

    ret.Clear();

    lldb::addr_t func_local_addr = LLDB_INVALID_ADDRESS;
    lldb::addr_t func_remote_addr = LLDB_INVALID_ADDRESS;

    for (JittedFunction &function : m_jitted_functions)
    {
        if (strstr(function.m_name.AsCString(), m_name.AsCString()))
        {
            func_local_addr = function.m_local_addr;
            func_remote_addr = function.m_remote_addr;
        }
    }

    if (func_local_addr == LLDB_INVALID_ADDRESS)
    {
        ret.SetErrorToGenericError();
        ret.SetErrorStringWithFormat("Couldn't find function %s for disassembly", m_name.AsCString());
        return ret;
    }

    if (log)
        log->Printf("Found function, has local address 0x%" PRIx64 " and remote address 0x%" PRIx64, (uint64_t)func_local_addr, (uint64_t)func_remote_addr);

    std::pair <lldb::addr_t, lldb::addr_t> func_range;

    func_range = GetRemoteRangeForLocal(func_local_addr);

    if (func_range.first == 0 && func_range.second == 0)
    {
        ret.SetErrorToGenericError();
        ret.SetErrorStringWithFormat("Couldn't find code range for function %s", m_name.AsCString());
        return ret;
    }

    if (log)
        log->Printf("Function's code range is [0x%" PRIx64 "+0x%" PRIx64 "]", func_range.first, func_range.second);

    Target *target = exe_ctx.GetTargetPtr();
    if (!target)
    {
        ret.SetErrorToGenericError();
        ret.SetErrorString("Couldn't find the target");
        return ret;
    }

    lldb::DataBufferSP buffer_sp(new DataBufferHeap(func_range.second, 0));

    Process *process = exe_ctx.GetProcessPtr();
    Error err;
    process->ReadMemory(func_remote_addr, buffer_sp->GetBytes(), buffer_sp->GetByteSize(), err);

    if (!err.Success())
    {
        ret.SetErrorToGenericError();
        ret.SetErrorStringWithFormat("Couldn't read from process: %s", err.AsCString("unknown error"));
        return ret;
    }

    ArchSpec arch(target->GetArchitecture());

    const char *plugin_name = NULL;
    const char *flavor_string = NULL;
    lldb::DisassemblerSP disassembler_sp = Disassembler::FindPlugin(arch, flavor_string, plugin_name);

    if (!disassembler_sp)
    {
        ret.SetErrorToGenericError();
        ret.SetErrorStringWithFormat("Unable to find disassembler plug-in for %s architecture.", arch.GetArchitectureName());
        return ret;
    }

    if (!process)
    {
        ret.SetErrorToGenericError();
        ret.SetErrorString("Couldn't find the process");
        return ret;
    }

    DataExtractor extractor(buffer_sp,
                            process->GetByteOrder(),
                            target->GetArchitecture().GetAddressByteSize());

    if (log)
    {
        log->Printf("Function data has contents:");
        extractor.PutToLog (log,
                            0,
                            extractor.GetByteSize(),
                            func_remote_addr,
                            16,
                            DataExtractor::TypeUInt8);
    }

    disassembler_sp->DecodeInstructions (Address (func_remote_addr), extractor, 0, UINT32_MAX, false, false);

    InstructionList &instruction_list = disassembler_sp->GetInstructionList();
    instruction_list.Dump(&stream, true, true, &exe_ctx);
    
    // FIXME: The DisassemblerLLVMC has a reference cycle and won't go away if it has any active instructions.
    // I'll fix that but for now, just clear the list and it will go away nicely.
    disassembler_sp->GetInstructionList().Clear();
    return ret;
}

static void ReportInlineAsmError(const llvm::SMDiagnostic &diagnostic, void *Context, unsigned LocCookie)
{
    Error *err = static_cast<Error*>(Context);

    if (err && err->Success())
    {
        err->SetErrorToGenericError();
        err->SetErrorStringWithFormat("Inline assembly error: %s", diagnostic.getMessage().str().c_str());
    }
}

void
IRExecutionUnit::ReportSymbolLookupError(const ConstString &name)
{
    m_failed_lookups.push_back(name);
}

void
IRExecutionUnit::GetRunnableInfo(Error &error,
                                 lldb::addr_t &func_addr,
                                 lldb::addr_t &func_end)
{
    lldb::ProcessSP process_sp(GetProcessWP().lock());

    Mutex::Locker global_context_locker(IRExecutionUnit::GetLLVMGlobalContextMutex());
    
    func_addr = LLDB_INVALID_ADDRESS;
    func_end = LLDB_INVALID_ADDRESS;

    if (!process_sp)
    {
        error.SetErrorToGenericError();
        error.SetErrorString("Couldn't write the JIT compiled code into the process because the process is invalid");
        return;
    }

    if (m_did_jit)
    {
        func_addr = m_function_load_addr;
        func_end = m_function_end_load_addr;

        return;
    };
    
    m_did_jit = true;

    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));

    std::string error_string;

    if (log)
    {
        std::string s;
        llvm::raw_string_ostream oss(s);

        m_module->print(oss, NULL);

        oss.flush();

        log->Printf ("Module being sent to JIT: \n%s", s.c_str());
    }

    llvm::Triple triple(m_module->getTargetTriple());
    //llvm::Function *function = m_module->getFunction (m_name.AsCString());
    llvm::Reloc::Model relocModel;
    llvm::CodeModel::Model codeModel;

    if (triple.isOSBinFormatELF())
    {
        relocModel = llvm::Reloc::Static;
    }
    else
    {
        relocModel = llvm::Reloc::PIC_;
    }
    
    // This will be small for 32-bit and large for 64-bit.
    codeModel = llvm::CodeModel::JITDefault;

    m_module_ap->getContext().setInlineAsmDiagnosticHandler(ReportInlineAsmError, &error);

    llvm::EngineBuilder builder(std::move(m_module_ap));

    builder.setEngineKind(llvm::EngineKind::JIT)
    .setErrorStr(&error_string)
    .setRelocationModel(relocModel)
    .setMCJITMemoryManager(std::unique_ptr<MemoryManager>(new MemoryManager(*this)))
    .setCodeModel(codeModel)
    .setOptLevel(llvm::CodeGenOpt::None);

    llvm::StringRef mArch;
    llvm::StringRef mCPU;
    llvm::SmallVector<std::string, 0> mAttrs;

    for (std::string &feature : m_cpu_features)
        mAttrs.push_back(feature);

    llvm::TargetMachine *target_machine = builder.selectTarget(triple,
                                                               mArch,
                                                               mCPU,
                                                               mAttrs);

    m_execution_engine_ap.reset(builder.create(target_machine));
    
    m_strip_underscore = (m_execution_engine_ap->getDataLayout().getGlobalPrefix() == '_');

    if (!m_execution_engine_ap.get())
    {
        error.SetErrorToGenericError();
        error.SetErrorStringWithFormat("Couldn't JIT the function: %s", error_string.c_str());
        return;
    }

    // Make sure we see all sections, including ones that don't have relocations...
    m_execution_engine_ap->setProcessAllSections(true);

    m_execution_engine_ap->DisableLazyCompilation();
    
    for (llvm::Function &function : *m_module)
    {
        if (function.isDeclaration() || !(function.hasExternalLinkage() || function.hasLinkOnceODRLinkage()) )
            continue;
        
        void *fun_ptr = m_execution_engine_ap->getPointerToFunction(&function);
        
        if (!error.Success())
        {
            // We got an error through our callback!
            return;
        }
        
        if (!fun_ptr)
        {
            error.SetErrorToGenericError();
            error.SetErrorStringWithFormat("'%s' was in the JITted module but wasn't lowered", function.getName().str().c_str());
            return;
        }
        m_jitted_functions.push_back (JittedFunction(function.getName().str().c_str(), (lldb::addr_t)fun_ptr));
    }

    CommitAllocations(process_sp);
    ReportAllocations(*m_execution_engine_ap);

    // We have to do this after calling ReportAllocations because for the MCJIT, getGlobalValueAddress
    // will cause the JIT to perform all relocations.  That can only be done once, and has to happen
    // after we do the remapping from local -> remote.
    // That means we don't know the local address of the Variables, but we don't need that for anything,
    // so that's okay.
    
    std::function<void (llvm::GlobalValue &)> RegisterOneValue = [this] (llvm::GlobalValue &val) {
        if (val.hasExternalLinkage() && !val.isDeclaration())
        {
            uint64_t var_ptr_addr = m_execution_engine_ap->getGlobalValueAddress(val.getName().str());
            if (var_ptr_addr != 0)
                m_jitted_global_variables.push_back (JittedGlobalVariable (val.getName().str().c_str(), LLDB_INVALID_ADDRESS, (lldb::addr_t) var_ptr_addr));
        }
    };
    
    for (llvm::GlobalVariable &global_var : m_module->getGlobalList())
    {
        RegisterOneValue(global_var);
    }
    
    for (llvm::GlobalAlias &global_alias : m_module->getAliasList())
    {
        RegisterOneValue(global_alias);
    }
    
    WriteData(process_sp);
    
    if (m_failed_lookups.size())
    {
        StreamString ss;
        
        ss.PutCString("Couldn't lookup symbols:\n");
        
        bool emitNewLine = false;
        
        for (const ConstString &failed_lookup : m_failed_lookups)
        {
            if (emitNewLine)
                ss.PutCString("\n");
            emitNewLine = true;
            ss.PutCString("  ");
            ss.PutCString(Mangled(failed_lookup).GetDemangledName(lldb::eLanguageTypeObjC_plus_plus).AsCString());
        }
        
        m_failed_lookups.clear();
        
        error.SetErrorString(ss.GetData());
        
        return;
    }
    
    m_function_load_addr = LLDB_INVALID_ADDRESS;
    m_function_end_load_addr = LLDB_INVALID_ADDRESS;

    for (JittedFunction &jitted_function : m_jitted_functions)
    {
        jitted_function.m_remote_addr = GetRemoteAddressForLocal (jitted_function.m_local_addr);
        
        if (jitted_function.m_name == m_name)
        {
            AddrRange func_range = GetRemoteRangeForLocal (jitted_function.m_local_addr);
            m_function_end_load_addr = func_range.first + func_range.second;
            m_function_load_addr = jitted_function.m_remote_addr;
        }
    }
    
    if (m_function_load_addr == LLDB_INVALID_ADDRESS)
    {
        error.SetErrorToGenericError();
        error.SetErrorStringWithFormat("Couldn't find '%s' in the JITted module", m_name.AsCString());
        return;
    }
    
    if (log)
    {
        log->Printf("Code can be run in the target.");

        StreamString disassembly_stream;

        Error err = DisassembleFunction(disassembly_stream, process_sp);

        if (!err.Success())
        {
            log->Printf("Couldn't disassemble function : %s", err.AsCString("unknown error"));
        }
        else
        {
            log->Printf("Function disassembly:\n%s", disassembly_stream.GetData());
        }

        log->Printf("Sections: ");
        for (AllocationRecord &record : m_records)
        {
            if (record.m_process_address != LLDB_INVALID_ADDRESS)
            {
                record.dump(log);

                DataBufferHeap my_buffer(record.m_size, 0);
                Error err;
                ReadMemory(my_buffer.GetBytes(), record.m_process_address, record.m_size, err);

                if (err.Success())
                {
                    DataExtractor my_extractor(my_buffer.GetBytes(), my_buffer.GetByteSize(), lldb::eByteOrderBig, 8);
                    my_extractor.PutToLog(log, 0, my_buffer.GetByteSize(), record.m_process_address, 16, DataExtractor::TypeUInt8);
                }
            }
            else
            {
                record.dump(log);
                
                DataExtractor my_extractor ((const void*)record.m_host_address, record.m_size, lldb::eByteOrderBig, 8);
                my_extractor.PutToLog(log, 0, record.m_size, record.m_host_address, 16, DataExtractor::TypeUInt8);
            }
        }
    }

    func_addr = m_function_load_addr;
    func_end = m_function_end_load_addr;

    return;
}

IRExecutionUnit::MemoryManager::MemoryManager (IRExecutionUnit &parent) :
    m_default_mm_ap (new llvm::SectionMemoryManager()),
    m_parent (parent)
{
}

IRExecutionUnit::MemoryManager::~MemoryManager ()
{
}

lldb::SectionType
IRExecutionUnit::GetSectionTypeFromSectionName (const llvm::StringRef &name, IRExecutionUnit::AllocationKind alloc_kind)
{
    lldb::SectionType sect_type = lldb::eSectionTypeCode;
    switch (alloc_kind)
    {
        case AllocationKind::Stub:  sect_type = lldb::eSectionTypeCode; break;
        case AllocationKind::Code:  sect_type = lldb::eSectionTypeCode; break;
        case AllocationKind::Data:  sect_type = lldb::eSectionTypeData; break;
        case AllocationKind::Global:sect_type = lldb::eSectionTypeData; break;
        case AllocationKind::Bytes: sect_type = lldb::eSectionTypeOther; break;
    }

    if (!name.empty())
    {
        if (name.equals("__text") || name.equals(".text"))
            sect_type = lldb::eSectionTypeCode;
        else if (name.equals("__data") || name.equals(".data"))
            sect_type = lldb::eSectionTypeCode;
        else if (name.startswith("__debug_") || name.startswith(".debug_"))
        {
            const uint32_t name_idx = name[0] == '_' ? 8 : 7;
            llvm::StringRef dwarf_name(name.substr(name_idx));
            switch (dwarf_name[0])
            {
                case 'a':
                    if (dwarf_name.equals("abbrev"))
                        sect_type = lldb::eSectionTypeDWARFDebugAbbrev;
                    else if (dwarf_name.equals("aranges"))
                        sect_type = lldb::eSectionTypeDWARFDebugAranges;
                    else if (dwarf_name.equals("addr"))
                        sect_type = lldb::eSectionTypeDWARFDebugAddr;
                    break;

                case 'f':
                    if (dwarf_name.equals("frame"))
                        sect_type = lldb::eSectionTypeDWARFDebugFrame;
                    break;

                case 'i':
                    if (dwarf_name.equals("info"))
                        sect_type = lldb::eSectionTypeDWARFDebugInfo;
                    break;

                case 'l':
                    if (dwarf_name.equals("line"))
                        sect_type = lldb::eSectionTypeDWARFDebugLine;
                    else if (dwarf_name.equals("loc"))
                        sect_type = lldb::eSectionTypeDWARFDebugLoc;
                    break;

                case 'm':
                    if (dwarf_name.equals("macinfo"))
                        sect_type = lldb::eSectionTypeDWARFDebugMacInfo;
                    break;

                case 'p':
                    if (dwarf_name.equals("pubnames"))
                        sect_type = lldb::eSectionTypeDWARFDebugPubNames;
                    else if (dwarf_name.equals("pubtypes"))
                        sect_type = lldb::eSectionTypeDWARFDebugPubTypes;
                    break;

                case 's':
                    if (dwarf_name.equals("str"))
                        sect_type = lldb::eSectionTypeDWARFDebugStr;
                    else if (dwarf_name.equals("str_offsets"))
                        sect_type = lldb::eSectionTypeDWARFDebugStrOffsets;
                    break;

                case 'r':
                    if (dwarf_name.equals("ranges"))
                        sect_type = lldb::eSectionTypeDWARFDebugRanges;
                    break;

                default:
                    break;
            }
        }
        else if (name.startswith("__apple_") || name.startswith(".apple_"))
        {
#if 0
            const uint32_t name_idx = name[0] == '_' ? 8 : 7;
            llvm::StringRef apple_name(name.substr(name_idx));
            switch (apple_name[0])
            {
                case 'n':
                    if (apple_name.equals("names"))
                        sect_type = lldb::eSectionTypeDWARFAppleNames;
                    else if (apple_name.equals("namespac") || apple_name.equals("namespaces"))
                        sect_type = lldb::eSectionTypeDWARFAppleNamespaces;
                    break;
                case 't':
                    if (apple_name.equals("types"))
                        sect_type = lldb::eSectionTypeDWARFAppleTypes;
                    break;
                case 'o':
                    if (apple_name.equals("objc"))
                        sect_type = lldb::eSectionTypeDWARFAppleObjC;
                    break;
                default:
                    break;
            }
#else
            sect_type = lldb::eSectionTypeInvalid;
#endif
        }
        else if (name.equals("__objc_imageinfo"))
            sect_type = lldb::eSectionTypeOther;
    }
    return sect_type;
}

uint8_t *
IRExecutionUnit::MemoryManager::allocateCodeSection(uintptr_t Size,
                                                    unsigned Alignment,
                                                    unsigned SectionID,
                                                    llvm::StringRef SectionName)
{
    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));

    uint8_t *return_value = m_default_mm_ap->allocateCodeSection(Size, Alignment, SectionID, SectionName);

    m_parent.m_records.push_back(AllocationRecord((uintptr_t)return_value,
                                                  lldb::ePermissionsReadable | lldb::ePermissionsExecutable,
                                                  GetSectionTypeFromSectionName (SectionName, AllocationKind::Code),
                                                  Size,
                                                  Alignment,
                                                  SectionID,
                                                  SectionName.str().c_str()));

    if (log)
    {
        log->Printf("IRExecutionUnit::allocateCodeSection(Size=0x%" PRIx64 ", Alignment=%u, SectionID=%u) = %p",
                    (uint64_t)Size, Alignment, SectionID, (void *)return_value);
    }

    return return_value;
}

uint8_t *
IRExecutionUnit::MemoryManager::allocateDataSection(uintptr_t Size,
                                                    unsigned Alignment,
                                                    unsigned SectionID,
                                                    llvm::StringRef SectionName,
                                                    bool IsReadOnly)
{
    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));

    uint8_t *return_value = m_default_mm_ap->allocateDataSection(Size, Alignment, SectionID, SectionName, IsReadOnly);

    uint32_t permissions = lldb::ePermissionsReadable;
    if (!IsReadOnly)
        permissions |= lldb::ePermissionsWritable;
    m_parent.m_records.push_back(AllocationRecord((uintptr_t)return_value,
                                                  permissions,
                                                  GetSectionTypeFromSectionName (SectionName, AllocationKind::Data),
                                                  Size,
                                                  Alignment,
                                                  SectionID,
                                                  SectionName.str().c_str()));
    if (log)
    {
        log->Printf("IRExecutionUnit::allocateDataSection(Size=0x%" PRIx64 ", Alignment=%u, SectionID=%u) = %p",
                    (uint64_t)Size, Alignment, SectionID, (void *)return_value);
    }

    return return_value;
}

static ConstString
FindBestAlternateMangledName(const ConstString &demangled,
                             const lldb::LanguageType &lang_type,
                             const SymbolContext &sym_ctx)
{
    CPlusPlusLanguage::MethodName cpp_name(demangled);
    std::string scope_qualified_name = cpp_name.GetScopeQualifiedName();

    if (!scope_qualified_name.size())
        return ConstString();

    if (!sym_ctx.module_sp)
        return ConstString();

    SymbolVendor *sym_vendor = sym_ctx.module_sp->GetSymbolVendor();
    if (!sym_vendor)
        return ConstString();

    lldb_private::SymbolFile *sym_file = sym_vendor->GetSymbolFile();
    if (!sym_file)
        return ConstString();

    std::vector<ConstString> alternates;
    sym_file->GetMangledNamesForFunction(scope_qualified_name, alternates);

    std::vector<ConstString> param_and_qual_matches;
    std::vector<ConstString> param_matches;
    for (size_t i = 0; i < alternates.size(); i++)
    {
        ConstString alternate_mangled_name = alternates[i];
        Mangled mangled(alternate_mangled_name, true);
        ConstString demangled = mangled.GetDemangledName(lang_type);

        CPlusPlusLanguage::MethodName alternate_cpp_name(demangled);
        if (!cpp_name.IsValid())
            continue;

        if (alternate_cpp_name.GetArguments() == cpp_name.GetArguments())
        {
            if (alternate_cpp_name.GetQualifiers() == cpp_name.GetQualifiers())
                param_and_qual_matches.push_back(alternate_mangled_name);
            else
                param_matches.push_back(alternate_mangled_name);
        }
    }

    if (param_and_qual_matches.size())
        return param_and_qual_matches[0]; // It is assumed that there will be only one!
    else if (param_matches.size())
        return param_matches[0]; // Return one of them as a best match
    else
        return ConstString();
}

struct IRExecutionUnit::SearchSpec
{
    ConstString name;
    uint32_t    mask;

    SearchSpec(ConstString n, uint32_t m = lldb::eFunctionNameTypeAuto) :
        name(n),
        mask(m)
    {
    }
};

void
IRExecutionUnit::CollectCandidateCNames(std::vector<IRExecutionUnit::SearchSpec> &C_specs, const ConstString &name)
{
    if (m_strip_underscore && name.AsCString()[0] == '_')
        C_specs.insert(C_specs.begin(), ConstString(&name.AsCString()[1]));
    C_specs.push_back(SearchSpec(name));
}

void
IRExecutionUnit::CollectCandidateCPlusPlusNames(std::vector<IRExecutionUnit::SearchSpec> &CPP_specs, const std::vector<SearchSpec> &C_specs, const SymbolContext &sc)
{
    for (const SearchSpec &C_spec : C_specs)
    {
        const ConstString &name = C_spec.name;

        if (CPlusPlusLanguage::IsCPPMangledName(name.GetCString()))
        {
            Mangled mangled(name, true);
            ConstString demangled = mangled.GetDemangledName(lldb::eLanguageTypeC_plus_plus);
            
            if (demangled)
            {
                ConstString best_alternate_mangled_name = FindBestAlternateMangledName(demangled, lldb::eLanguageTypeC_plus_plus, sc);
                
                if (best_alternate_mangled_name)
                {
                    CPP_specs.push_back(best_alternate_mangled_name);
                }
                
                CPP_specs.push_back(SearchSpec(demangled, lldb::eFunctionNameTypeFull));
            }
        }
        
        // Maybe we're looking for a const symbol but the debug info told us it was const...
        if (!strncmp(name.GetCString(), "_ZN", 3) &&
            strncmp(name.GetCString(), "_ZNK", 4))
        {
            std::string fixed_scratch("_ZNK");
            fixed_scratch.append(name.GetCString() + 3);
            CPP_specs.push_back(ConstString(fixed_scratch.c_str()));
        }
        
        // Maybe we're looking for a static symbol but we thought it was global...
        if (!strncmp(name.GetCString(), "_Z", 2) &&
            strncmp(name.GetCString(), "_ZL", 3))
        {
            std::string fixed_scratch("_ZL");
            fixed_scratch.append(name.GetCString() + 2);
            CPP_specs.push_back(ConstString(fixed_scratch.c_str()));
        }

    }
}

lldb::addr_t
IRExecutionUnit::FindInSymbols(const std::vector<IRExecutionUnit::SearchSpec> &specs, const lldb_private::SymbolContext &sc)
{
    Target *target = sc.target_sp.get();

    if (!target)
    {
        // we shouldn't be doing any symbol lookup at all without a target
        return LLDB_INVALID_ADDRESS;
    }

    for (const SearchSpec &spec : specs)
    {
        SymbolContextList sc_list;

        lldb::addr_t best_internal_load_address = LLDB_INVALID_ADDRESS;

        std::function<bool (lldb::addr_t &, SymbolContextList &, const lldb_private::SymbolContext &)> get_external_load_address =
            [&best_internal_load_address, target](lldb::addr_t &load_address,
                                                  SymbolContextList &sc_list,
                                                  const lldb_private::SymbolContext &sc) -> lldb::addr_t
        {
            load_address = LLDB_INVALID_ADDRESS;

            for (size_t si = 0, se = sc_list.GetSize(); si < se; ++si)
            {
                SymbolContext candidate_sc;

                sc_list.GetContextAtIndex(si, candidate_sc);

                const bool is_external = (candidate_sc.function) ||
                                         (candidate_sc.symbol && candidate_sc.symbol->IsExternal());

                AddressRange range;

                if (candidate_sc.GetAddressRange(lldb::eSymbolContextFunction | lldb::eSymbolContextSymbol,
                                                 0,
                                                 false,
                                                 range))
                {
                    load_address = range.GetBaseAddress().GetCallableLoadAddress(target);

                    if (load_address != LLDB_INVALID_ADDRESS)
                    {
                        if (is_external)
                        {
                            return true;
                        }
                        else if (best_internal_load_address == LLDB_INVALID_ADDRESS)
                        {
                            best_internal_load_address = load_address;
                            load_address = LLDB_INVALID_ADDRESS;
                        }
                    }
                }
            }

            return false;
        };
        
        if (sc.module_sp)
        {
            sc.module_sp->FindFunctions(spec.name,
                                        NULL,
                                        spec.mask,
                                        true,  // include_symbols
                                        false, // include_inlines
                                        true,  // append
                                        sc_list);
        }

        lldb::addr_t load_address = LLDB_INVALID_ADDRESS;

        if (get_external_load_address(load_address, sc_list, sc))
        {
            return load_address;
        }
        else
        {
            sc_list.Clear();
        }
    
        if (sc_list.GetSize() == 0 && sc.target_sp)
        {
            sc.target_sp->GetImages().FindFunctions(spec.name,
                                                    spec.mask,
                                                    true,  // include_symbols
                                                    false, // include_inlines
                                                    true,  // append
                                                    sc_list);
        }

        if (get_external_load_address(load_address, sc_list, sc))
        {
            return load_address;
        }
        else
        {
            sc_list.Clear();
        }

        if (sc_list.GetSize() == 0 && sc.target_sp)
        {
            sc.target_sp->GetImages().FindSymbolsWithNameAndType(spec.name, lldb::eSymbolTypeAny, sc_list);
        }

        if (get_external_load_address(load_address, sc_list, sc))
        {
            return load_address;
        }
        // if there are any searches we try after this, add an sc_list.Clear() in an "else" clause here

        if (best_internal_load_address != LLDB_INVALID_ADDRESS)
        {
            return best_internal_load_address;
        }
    }

    return LLDB_INVALID_ADDRESS;
}

lldb::addr_t
IRExecutionUnit::FindInRuntimes(const std::vector<SearchSpec> &specs, const lldb_private::SymbolContext &sc)
{
    lldb::TargetSP target_sp = sc.target_sp;
    
    if (!target_sp)
    {
        return LLDB_INVALID_ADDRESS;
    }
    
    lldb::ProcessSP process_sp = sc.target_sp->GetProcessSP();
    
    if (!process_sp)
    {
        return LLDB_INVALID_ADDRESS;
    }

    ObjCLanguageRuntime *runtime = process_sp->GetObjCLanguageRuntime();

    if (runtime)
    {
        for (const SearchSpec &spec : specs)
        {
            lldb::addr_t symbol_load_addr = runtime->LookupRuntimeSymbol(spec.name);
            
            if (symbol_load_addr != LLDB_INVALID_ADDRESS)
                return symbol_load_addr;
        }
    }

    return LLDB_INVALID_ADDRESS;
}

lldb::addr_t
IRExecutionUnit::FindInUserDefinedSymbols(const std::vector<SearchSpec> &specs, const lldb_private::SymbolContext &sc)
{
    lldb::TargetSP target_sp = sc.target_sp;

    for (const SearchSpec &spec : specs)
    {
        lldb::addr_t symbol_load_addr = target_sp->GetPersistentSymbol(spec.name);

        if (symbol_load_addr != LLDB_INVALID_ADDRESS)
            return symbol_load_addr;
    }

    return LLDB_INVALID_ADDRESS;
}

lldb::addr_t
IRExecutionUnit::FindSymbol(const lldb_private::ConstString &name)
{
    std::vector<SearchSpec> candidate_C_names;
    std::vector<SearchSpec> candidate_CPlusPlus_names;
    
    CollectCandidateCNames(candidate_C_names, name);

    lldb::addr_t ret = FindInSymbols(candidate_C_names, m_sym_ctx);
    if (ret == LLDB_INVALID_ADDRESS)
        ret = FindInRuntimes(candidate_C_names, m_sym_ctx);

    if (ret == LLDB_INVALID_ADDRESS)
        ret = FindInUserDefinedSymbols(candidate_C_names, m_sym_ctx);

    if (ret == LLDB_INVALID_ADDRESS)
    {
        CollectCandidateCPlusPlusNames(candidate_CPlusPlus_names, candidate_C_names, m_sym_ctx);
        ret = FindInSymbols(candidate_CPlusPlus_names, m_sym_ctx);
    }

    return ret;
}

uint64_t
IRExecutionUnit::MemoryManager::getSymbolAddress(const std::string &Name)
{
    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));

    ConstString name_cs(Name.c_str());
    
    lldb::addr_t ret = m_parent.FindSymbol(name_cs);
    
    if (ret == LLDB_INVALID_ADDRESS)
    {
        if (log)
            log->Printf("IRExecutionUnit::getSymbolAddress(Name=\"%s\") = <not found>",
                        Name.c_str());

        m_parent.ReportSymbolLookupError(name_cs);
        return 0xbad0bad0;
    }
    else
    {
        if (log)
            log->Printf("IRExecutionUnit::getSymbolAddress(Name=\"%s\") = %" PRIx64,
                        Name.c_str(),
                        ret);
        return ret;
    }
}

void *
IRExecutionUnit::MemoryManager::getPointerToNamedFunction(const std::string &Name,
                                                          bool AbortOnFailure) {
    assert (sizeof(void *) == 8);
    
    return (void*)getSymbolAddress(Name);
}

lldb::addr_t
IRExecutionUnit::GetRemoteAddressForLocal (lldb::addr_t local_address)
{
    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));

    for (AllocationRecord &record : m_records)
    {
        if (local_address >= record.m_host_address &&
            local_address < record.m_host_address + record.m_size)
        {
            if (record.m_process_address == LLDB_INVALID_ADDRESS)
                return LLDB_INVALID_ADDRESS;

            lldb::addr_t ret = record.m_process_address + (local_address - record.m_host_address);

            if (log)
            {
                log->Printf("IRExecutionUnit::GetRemoteAddressForLocal() found 0x%" PRIx64 " in [0x%" PRIx64 "..0x%" PRIx64 "], and returned 0x%" PRIx64 " from [0x%" PRIx64 "..0x%" PRIx64 "].",
                            local_address,
                            (uint64_t)record.m_host_address,
                            (uint64_t)record.m_host_address + (uint64_t)record.m_size,
                            ret,
                            record.m_process_address,
                            record.m_process_address + record.m_size);
            }

            return ret;
        }
    }

    return LLDB_INVALID_ADDRESS;
}

IRExecutionUnit::AddrRange
IRExecutionUnit::GetRemoteRangeForLocal (lldb::addr_t local_address)
{
    for (AllocationRecord &record : m_records)
    {
        if (local_address >= record.m_host_address &&
            local_address < record.m_host_address + record.m_size)
        {
            if (record.m_process_address == LLDB_INVALID_ADDRESS)
                return AddrRange(0, 0);

            return AddrRange(record.m_process_address, record.m_size);
        }
    }

    return AddrRange (0, 0);
}

bool
IRExecutionUnit::CommitAllocations (lldb::ProcessSP &process_sp)
{
    bool ret = true;

    lldb_private::Error err;

    for (AllocationRecord &record : m_records)
    {
        if (record.m_process_address != LLDB_INVALID_ADDRESS)
            continue;

        switch (record.m_sect_type)
        {
        case lldb::eSectionTypeInvalid:
        case lldb::eSectionTypeDWARFDebugAbbrev:
        case lldb::eSectionTypeDWARFDebugAddr:
        case lldb::eSectionTypeDWARFDebugAranges:
        case lldb::eSectionTypeDWARFDebugFrame:
        case lldb::eSectionTypeDWARFDebugInfo:
        case lldb::eSectionTypeDWARFDebugLine:
        case lldb::eSectionTypeDWARFDebugLoc:
        case lldb::eSectionTypeDWARFDebugMacInfo:
        case lldb::eSectionTypeDWARFDebugPubNames:
        case lldb::eSectionTypeDWARFDebugPubTypes:
        case lldb::eSectionTypeDWARFDebugRanges:
        case lldb::eSectionTypeDWARFDebugStr:
        case lldb::eSectionTypeDWARFDebugStrOffsets:
        case lldb::eSectionTypeDWARFAppleNames:
        case lldb::eSectionTypeDWARFAppleTypes:
        case lldb::eSectionTypeDWARFAppleNamespaces:
        case lldb::eSectionTypeDWARFAppleObjC:
            err.Clear();
            break;
        default:
            const bool zero_memory = false;
            record.m_process_address = Malloc (record.m_size,
                                               record.m_alignment,
                                               record.m_permissions,
                                               eAllocationPolicyProcessOnly,
                                               zero_memory,
                                               err);
            break;
        }

        if (!err.Success())
        {
            ret = false;
            break;
        }
    }

    if (!ret)
    {
        for (AllocationRecord &record : m_records)
        {
            if (record.m_process_address != LLDB_INVALID_ADDRESS)
            {
                Free(record.m_process_address, err);
                record.m_process_address = LLDB_INVALID_ADDRESS;
            }
        }
    }

    return ret;
}

void
IRExecutionUnit::ReportAllocations (llvm::ExecutionEngine &engine)
{
    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));

    for (AllocationRecord &record : m_records)
    {
        if (record.m_process_address == LLDB_INVALID_ADDRESS)
            continue;

        if (record.m_section_id == eSectionIDInvalid)
            continue;

        engine.mapSectionAddress((void*)record.m_host_address, record.m_process_address);
        if (log)
            log->Printf("IRExecutionUnit::ReportAllocations() called mapSectionAddress(): [0x%" PRIx64 "..0x%" PRIx64 "] -> [0x%" PRIx64 "..0x%" PRIx64 "].",
                 (uint64_t)record.m_host_address,
                 (uint64_t)record.m_host_address + (uint64_t)record.m_size,
                 record.m_process_address,
                 record.m_process_address + record.m_size);
    }

    // Trigger re-application of relocations.
    engine.finalizeObject();
}

bool
IRExecutionUnit::WriteData (lldb::ProcessSP &process_sp)
{
    bool wrote_something = false;
    for (AllocationRecord &record : m_records)
    {
        if (record.m_process_address != LLDB_INVALID_ADDRESS)
        {
            lldb_private::Error err;
            WriteMemory (record.m_process_address, (uint8_t*)record.m_host_address, record.m_size, err);
            if (err.Success())
                wrote_something = true;
        }
    }
    return wrote_something;
}

void
IRExecutionUnit::AllocationRecord::dump (Log *log)
{
    if (!log)
        return;

    log->Printf("[0x%llx+0x%llx]->0x%llx (alignment %d, section ID %d, name %s)",
                (unsigned long long)m_host_address,
                (unsigned long long)m_size,
                (unsigned long long)m_process_address,
                (unsigned)m_alignment,
                (unsigned)m_section_id,
                m_name.c_str());
}


lldb::ByteOrder
IRExecutionUnit::GetByteOrder () const
{
    ExecutionContext exe_ctx (GetBestExecutionContextScope());
    return exe_ctx.GetByteOrder();
}

uint32_t
IRExecutionUnit::GetAddressByteSize () const
{
    ExecutionContext exe_ctx (GetBestExecutionContextScope());
    return exe_ctx.GetAddressByteSize();
}

void
IRExecutionUnit::PopulateSymtab (lldb_private::ObjectFile *obj_file,
                                 lldb_private::Symtab &symtab)
{
    if (m_execution_engine_ap)
    {
        uint32_t symbol_id = 0;
        lldb_private::SectionList *section_list = obj_file->GetSectionList();
        for (llvm::Function &function : *m_module)
        {
            if (function.isDeclaration() || !(function.hasExternalLinkage() || function.hasLinkOnceODRLinkage()) )
                continue;

            const lldb::addr_t function_addr = (intptr_t)m_execution_engine_ap->getPointerToFunction(&function);

            if (function_addr != 0)
            {
                lldb::SectionSP section_sp (section_list->FindSectionContainingFileAddress(function_addr));
                const lldb::addr_t section_addr = section_sp ? section_sp->GetFileAddress() : 0;
                const lldb::addr_t function_offset = function_addr - section_addr;
                llvm::GlobalValue::LinkageTypes linkage = function.getLinkage();
                llvm::StringRef function_name_ref = function.getName();
                Symbol symbol (++symbol_id,
                               function_name_ref.str().c_str(),
                               function_name_ref.startswith("_T") || function_name_ref.startswith("_Z"), // name_is_mangled
                               lldb::eSymbolTypeCode,
                               linkage == llvm::GlobalValue::ExternalLinkage, //  external
                               false,           // is_debug,
                               false,           // is_trampoline,
                               false,           // is_artificial,
                               section_sp,      // section
                               function_offset, // offset
                               0,               // Don't know the size of functions that I know of
                               false,           // size_is_valid
                               false,           // contains_linker_annotations
                               0);              // flags
                symbol.SetType(ObjectFile::GetSymbolTypeFromName(symbol.GetMangled().GetMangledName().GetStringRef(), symbol.GetType()));
                symtab.AddSymbol(symbol);
            }
        }

        for (llvm::GlobalVariable &global_var : m_module->getGlobalList())
        {
            if (global_var.isDeclaration() || !(global_var.hasExternalLinkage() || global_var.hasLinkOnceODRLinkage()))
                continue;
            llvm::StringRef global_name = global_var.getName();
            if (global_name.empty())
                continue;
            const lldb::addr_t global_addr = m_execution_engine_ap->getGlobalValueAddress(global_name.str());
            if (global_addr != 0)
            {
                lldb::SectionSP section_sp (section_list->FindSectionContainingFileAddress(global_addr));
                const lldb::addr_t section_addr = section_sp ? section_sp->GetFileAddress() : 0;
                const lldb::addr_t global_offset = global_addr - section_addr;
                llvm::StringRef global_name_ref = global_var.getName();
                Symbol symbol (++symbol_id,
                               global_name_ref.str().c_str(),
                               global_name_ref.startswith("_T") || global_name_ref.startswith("_Z"), // name_is_mangled
                               lldb::eSymbolTypeData,
                               global_var.hasExternalLinkage(), // is_external
                               false,           // is_debug,
                               false,           // is_trampoline,
                               false,           // is_artificial,
                               section_sp,      // section
                               global_offset,   // offset
                               0,               // Don't know the size of functions that I know of
                               false,           // size_is_valid
                               false,           // contains_linker_annotations
                               0);              // flags
                symbol.SetType(ObjectFile::GetSymbolTypeFromName(symbol.GetMangled().GetMangledName().GetStringRef(), symbol.GetType()));
                symtab.AddSymbol(symbol);
            }
        }
        symtab.CalculateSymbolSizes();
    }
}


void
IRExecutionUnit::PopulateSectionList (lldb_private::ObjectFile *obj_file,
                                      lldb_private::SectionList &section_list)
{
    for (AllocationRecord &record : m_records)
    {
        if (record.m_size > 0)
        {
            lldb::SectionSP section_sp (new lldb_private::Section (obj_file->GetModule(),
                                                                   obj_file,
                                                                   record.m_section_id,
                                                                   ConstString(record.m_name),
                                                                   record.m_sect_type,
                                                                   record.m_process_address,
                                                                   record.m_size,
                                                                   record.m_host_address,   // file_offset (which is the host address for the data)
                                                                   record.m_size,           // file_size
                                                                   0,
                                                                   record.m_permissions));  // flags
            section_list.AddSection (section_sp);
        }
    }
}

bool
IRExecutionUnit::GetArchitecture (lldb_private::ArchSpec &arch)
{
    ExecutionContext exe_ctx (GetBestExecutionContextScope());
    Target *target = exe_ctx.GetTargetPtr();
    if (target)
        arch = target->GetArchitecture();
    else
        arch.Clear();
    return arch.IsValid();
}

lldb::ModuleSP
IRExecutionUnit::GetJITModule ()
{
    // Accessor only, might return empty shared pointer
    return m_jit_module_wp.lock();
}


lldb::ModuleSP
IRExecutionUnit::CreateJITModule (const char *name,
                                  const FileSpec *limit_file_ptr,
                                  uint32_t limit_start_line,
                                  uint32_t limit_end_line)
{
    lldb::ModuleSP jit_module_sp (m_jit_module_wp.lock());
    if (jit_module_sp)
        return jit_module_sp;
    
    // Only create a JIT module if we are going to run it in the target
    if (m_execution_engine_ap)
    {
        ExecutionContext exe_ctx (GetBestExecutionContextScope());
        Target *target = exe_ctx.GetTargetPtr();
        if (target)
        {
            jit_module_sp = lldb_private::Module::CreateJITModule (std::static_pointer_cast<lldb_private::ObjectFileJITDelegate>(shared_from_this()));
            if (jit_module_sp)
            {
                m_jit_module_wp = jit_module_sp;
                bool changed = false;
                jit_module_sp->SetLoadAddress(*target, 0, true, changed);

                jit_module_sp->SetTypeSystemMap(target->GetTypeSystemMap());
                
                ConstString const_name(name);
                FileSpec jit_file;
                jit_file.GetFilename() = const_name;
                jit_module_sp->SetFileSpecAndObjectName (jit_file, ConstString());
                
                if (limit_file_ptr)
                {
                    SymbolVendor *symbol_vendor = jit_module_sp->GetSymbolVendor();
                    if (symbol_vendor)
                        symbol_vendor->SetLimitSourceFileRange (*limit_file_ptr, limit_start_line, limit_end_line);
                }

                target->GetImages().Append(jit_module_sp);
            }
            return jit_module_sp;
        }
    }
    return lldb::ModuleSP();
}

Mutex &
IRExecutionUnit::GetLLVMGlobalContextMutex ()
{
    static Mutex s_llvm_context_mutex(Mutex::Type::eMutexTypeRecursive);
    
    return s_llvm_context_mutex;
}

