#include "DriverEntry.h"
#include "ProcessFunc.h"
#include "commonDef.h"
#include <wdm.h>
#include <ntddk.h>
#include <ntstrsafe.h>
#include <minwindef.h>
#include <ntimage.h>

int i = 0;//����
ULONG ithread = 0;

int idtnum = 0;//idt
int gdtnum = 0;//gdt
DWORD driverNum = 0;
DWORD ssdtnum = 0;
LDR_DATA_TABLE_ENTRY* pLdrEntry;//����ģ��
struct _PEB* peb;//����ģ��
ULONG modulePID = 0;//�����ĸ����̵�ģ��

PEPROCESS threadPeprocess = NULL;//�����߳�
PDRIVER_OBJECT g_DriverObg;//��������
PLDR_DATA_TABLE_ENTRY pLdr;//��������
LIST_ENTRY* pTemp;//��������
int isSysHook = 0;//�Ƿ�sysenter hook
ULONG_PTR	g_oldKiFastCallEntery;
ULONG g_uPid = 0;//���������̵�PID
// ��������ԭ��

int isReload = 0;

////////////////////////////////////////////
/////////////  �ں�������غ���   /////////////
////////////////////////////////////////////

static char*		g_pNewNtKernel;		// ���ں�
static ULONG		g_ntKernelSize;		// �ں˵�ӳ���С
static SSDTEntry*	g_pNewSSDTEntry;	// ��ssdt����ڵ�ַ	
static ULONG		g_hookAddr;			// ��hookλ�õ��׵�ַ
static ULONG		g_hookAddr_next_ins;// ��hook��ָ�����һ��ָ����׵�ַ.
void _declspec(naked) disablePageWriteProtect()
{
	_asm
	{
		push eax;
		mov eax, cr0;
		and eax, ~0x10000;
		mov cr0, eax;
		pop eax;
		ret;
	}
}

// �����ڴ�ҳд�뱣��
void _declspec(naked) enablePageWriteProtect()
{
	_asm
	{
		push eax;
		mov eax, cr0;
		or eax, 0x10000;
		mov cr0, eax;
		pop eax;
		ret;
	}
}


// ����NT�ں�ģ��
// ����ȡ���Ļ����������ݱ��浽pBuff��.
NTSTATUS loadNtKernelModule(UNICODE_STRING* ntkernelPath, char** pOut)
{
	NTSTATUS status = STATUS_SUCCESS;
	// 2. ��ȡ�ļ��е��ں�ģ��
	// 2.1 ���ں�ģ����Ϊ�ļ�����.
	HANDLE hFile = NULL;
	char* pBuff = NULL;
	ULONG read = 0;
	char pKernelBuff[0x1000];

	status = createFile(ntkernelPath->Buffer,
		GENERIC_READ,
		FILE_SHARE_READ,
		FILE_OPEN_IF,
		FALSE,
		&hFile);
	if (status != STATUS_SUCCESS)
	{
		KdPrint(("���ļ�ʧ��\n"));
		goto _DONE;
	}

	// 2.2 ��PE�ļ�ͷ����ȡ���ڴ�
	status = readFile(hFile, 0, 0, 0x1000, pKernelBuff, &read);
	if (STATUS_SUCCESS != status)
	{
		KdPrint(("��ȡ�ļ�����ʧ��\n"));
		goto _DONE;
	}

	// 3. ����PE�ļ����ڴ�.
	// 3.1 �õ���չͷ,��ȡӳ���С. 
	IMAGE_DOS_HEADER* pDos = (IMAGE_DOS_HEADER*)pKernelBuff;
	IMAGE_NT_HEADERS* pnt = (IMAGE_NT_HEADERS*)((ULONG)pDos + pDos->e_lfanew);
	ULONG imgSize = pnt->OptionalHeader.SizeOfImage;

	// 3.2 �����ڴ��Ա���������ε�����.
	pBuff = ExAllocatePool(NonPagedPool, imgSize);
	if (pBuff == NULL)
	{
		KdPrint(("�ڴ�����ʧ��ʧ��\n"));
		status = STATUS_BUFFER_ALL_ZEROS;//��㷵�ظ�������
		goto _DONE;
	}

	// 3.2.1 ����ͷ�����ѿռ�
	RtlCopyMemory(pBuff,
		pKernelBuff,
		pnt->OptionalHeader.SizeOfHeaders);

	// 3.3 �õ�����ͷ, ������������ͷ�����ζ�ȡ���ڴ���.
	IMAGE_SECTION_HEADER* pScnHdr = IMAGE_FIRST_SECTION(pnt);
	ULONG scnCount = pnt->FileHeader.NumberOfSections;
	for (ULONG i = 0; i < scnCount; ++i)
	{
		//
		// 3.3.1 ��ȡ�ļ����ݵ��ѿռ�ָ��λ��.
		//
		status = readFile(hFile,
			pScnHdr[i].PointerToRawData,
			0,
			pScnHdr[i].SizeOfRawData,
			pScnHdr[i].VirtualAddress + pBuff,
			&read);
		if (status != STATUS_SUCCESS)
			goto _DONE;

	}


_DONE:
	ZwClose(hFile);
	//
	// �������ں˵ļ��ص��׵�ַ
	//
	*pOut = pBuff;

	if (status != STATUS_SUCCESS)
	{
		if (pBuff != NULL)
		{
			ExFreePool(pBuff);
			*pOut = pBuff = NULL;
		}
	}
	return status;
}


// �޸��ض�λ.
void fixRelocation(char* pDosHdr, ULONG curNtKernelBase)
{
	IMAGE_DOS_HEADER* pDos = (IMAGE_DOS_HEADER*)pDosHdr;
	IMAGE_NT_HEADERS* pNt = (IMAGE_NT_HEADERS*)((ULONG)pDos + pDos->e_lfanew);
	ULONG uRelRva = pNt->OptionalHeader.DataDirectory[5].VirtualAddress;
	IMAGE_BASE_RELOCATION* pRel =
		(IMAGE_BASE_RELOCATION*)(uRelRva + (ULONG)pDos);

	while (pRel->SizeOfBlock)
	{
		typedef struct
		{
			USHORT offset : 12;
			USHORT type : 4;
		}TypeOffset;

		ULONG count = (pRel->SizeOfBlock - 8) / 2;
		TypeOffset* pTypeOffset = (TypeOffset*)(pRel + 1);
		for (ULONG i = 0; i < count; ++i)
		{
			if (pTypeOffset[i].type != 3)
			{
				continue;
			}

			ULONG* pFixAddr = (ULONG*)(pTypeOffset[i].offset + pRel->VirtualAddress + (ULONG)pDos);
			//
			// ��ȥĬ�ϼ��ػ�ַ
			//
			*pFixAddr -= pNt->OptionalHeader.ImageBase;

			//
			// �����µļ��ػ�ַ(ʹ�õ��ǵ�ǰ�ں˵ļ��ػ�ַ,������
			// ��Ϊ�������ں�ʹ�õ�ǰ�ں˵�����(ȫ�ֱ���,δ��ʼ������������).)
			//
			*pFixAddr += (ULONG)curNtKernelBase;
		}

		pRel = (IMAGE_BASE_RELOCATION*)((ULONG)pRel + pRel->SizeOfBlock);
	}
}

// ���SSDT��
// char* pNewBase - �¼��ص��ں˶ѿռ��׵�ַ
// char* pCurKernelBase - ��ǰ����ʹ�õ��ں˵ļ��ػ�ַ
void initSSDT(char* pNewBase, char* pCurKernelBase)
{
	// 1. �ֱ��ȡ��ǰ�ں�,�¼��ص��ں˵�`KeServiceDescriptorTable`
	//    �ĵ�ַ
	SSDTEntry* pCurSSDTEnt = KeServiceDescriptorTable;
	g_pNewSSDTEntry = (SSDTEntry*)
		((ULONG)pCurSSDTEnt - (ULONG)pCurKernelBase + (ULONG)pNewBase);

	// 2. ��ȡ�¼��ص��ں��������ű�ĵ�ַ:
	// 2.1 ���������ַ
	g_pNewSSDTEntry->ServiceTableBase = (ULONG*)
		((ULONG)pCurSSDTEnt->ServiceTableBase - (ULONG)pCurKernelBase + (ULONG)pNewBase);

	// 2.3 �����������ֽ������ַ
	g_pNewSSDTEntry->ParamTableBase = (ULONG*)
		((ULONG)pCurSSDTEnt->ParamTableBase - (ULONG)pCurKernelBase + (ULONG)pNewBase);

	// 2.2 ���������ô������ַ(��ʱ�������������.)
	if (pCurSSDTEnt->ServiceCounterTableBase)
	{
		g_pNewSSDTEntry->ServiceCounterTableBase = (ULONG*)
			((ULONG)pCurSSDTEnt->ServiceCounterTableBase - (ULONG)pCurKernelBase + (ULONG)pNewBase);
	}

	// 2.3 ������SSDT��ķ������
	g_pNewSSDTEntry->NumberOfService = pCurSSDTEnt->NumberOfService;

	//3. ���������ĵ�ַ��䵽��SSDT��(�ض�λʱ��ʵ�Ѿ��޸�����,
	//   ����,���޸��ض�λ��ʱ��,��ʹ�õ�ǰ�ں˵ļ��ػ�ַ��, �޸��ض�λ
	//   Ϊ֮��, ���ں˵�SSDT����ķ������ĵ�ַ���ǵ�ǰ�ں˵ĵ�ַ,
	//   ������Ҫ����Щ�������ĵ�ַ�Ļ����ں��еĺ�����ַ.)
	disablePageWriteProtect();
	for (ULONG i = 0; i < g_pNewSSDTEntry->NumberOfService; ++i)
	{
		// ��ȥ��ǰ�ں˵ļ��ػ�ַ
		g_pNewSSDTEntry->ServiceTableBase[i] -= (ULONG)pCurKernelBase;
		// �������ں˵ļ��ػ�ַ.
		g_pNewSSDTEntry->ServiceTableBase[i] += (ULONG)pNewBase;
	}
	enablePageWriteProtect();
}
void uninstallHook()
{
	if (g_hookAddr)
	{

		// ��ԭʼ����д��.
		UCHAR srcCode[5] = { 0x2b,0xe1,0xc1,0xe9,0x02 };
		disablePageWriteProtect();

		// 3.1 ������תƫ��
		// ��תƫ�� = Ŀ���ַ - ��ǰ��ַ - 5

		_asm sti
		// 3.2 ����תָ��д��
		RtlCopyMemory(g_hookAddr,
			srcCode,
			5);
		_asm cli
		g_hookAddr = 0;
		enablePageWriteProtect();
	}

	if (g_pNewNtKernel)
	{
		ExFreePool(g_pNewNtKernel);
		g_pNewNtKernel = NULL;
	}
}
ULONG SSDTFilter(ULONG index,/*������,Ҳ�ǵ��ú�*/
	ULONG tableAddress,/*��ĵ�ַ,������SSDT��ĵ�ַ,Ҳ������Shadow SSDT��ĵ�ַ*/
	ULONG funAddr/*�ӱ���ȡ���ĺ�����ַ*/)
{
	// �����SSDT��Ļ�
	if (tableAddress == KeServiceDescriptorTable->ServiceTableBase)
	{
		// �жϵ��ú�(190��ZwOpenProcess�����ĵ��ú�)
		if (index == 190)
		{
			// ������SSDT��ĺ�����ַ
			// Ҳ�������ں˵ĺ�����ַ.
			return g_pNewSSDTEntry->ServiceTableBase[190];
		}
	}
	// ���ؾɵĺ�����ַ
	return funAddr;
}
void _declspec(naked) myKiFastEntryHook()
{

	_asm
	{
		pushad; // ѹջ�Ĵ���: eax,ecx,edx,ebx, esp,ebp ,esi, edi
		pushfd; // ѹջ��־�Ĵ���

		push edx; // �ӱ���ȡ���ĺ�����ַ
		push edi; // ��ĵ�ַ
		push eax; // ���ú�
		call SSDTFilter; // ���ù��˺���

		;// �����������֮��ջ�ؼ�����,ָ��pushad��
		;// 32λ��ͨ�üĴ���������ջ��,ջ�ռ䲼��Ϊ:
		;// [esp + 00] <=> eflag
		;// [esp + 04] <=> edi
		;// [esp + 08] <=> esi
		;// [esp + 0C] <=> ebp
		;// [esp + 10] <=> esp
		;// [esp + 14] <=> ebx
		;// [esp + 18] <=> edx <<-- ʹ�ú�������ֵ���޸����λ��
		;// [esp + 1C] <=> ecx
		;// [esp + 20] <=> eax
		mov dword ptr ds : [esp + 0x18], eax;
		popfd; // popfdʱ,ʵ����edx��ֵ�ͻر��޸�
		popad;

		; //ִ�б�hook���ǵ�����ָ��
		sub esp, ecx;
		shr ecx, 2;
		jmp g_hookAddr_next_ins;
	}
}
void installHook()
{
	g_hookAddr = 0;

	// 1. �ҵ�KiFastCallEntry�����׵�ַ
	ULONG uKiFastCallEntry = 0;
	_asm
	{
		;// KiFastCallEntry������ַ����
		;// ������ģ��Ĵ�����0x176�żĴ�����
		push ecx;
		push eax;
		push edx;
		mov ecx, 0x176; // ���ñ��
		rdmsr; ;// ��ȡ��edx:eax
		mov uKiFastCallEntry, eax;// ���浽����
		pop edx;
		pop eax;
		pop ecx;
	}

	// 2. �ҵ�HOOK��λ��, ������5�ֽڵ�����
	// 2.1 HOOK��λ��ѡ��Ϊ(�˴�����5�ֽ�,):
	//  2be1            sub     esp, ecx ;
	// 	c1e902          shr     ecx, 2   ;
	UCHAR hookCode[5] = { 0x2b,0xe1,0xc1,0xe9,0x02 }; //����inline hook���ǵ�5�ֽ�
	ULONG i = 0;
	for (; i < 0x1FF; ++i)
	{
		if (RtlCompareMemory((UCHAR*)uKiFastCallEntry + i,
			hookCode,
			5) == 5)
		{
			break;
		}
	}
	if (i >= 0x1FF)
	{
		KdPrint(("��KiFastCallEntry������û���ҵ�HOOKλ��,����KiFastCallEntry�Ѿ���HOOK����\n"));
		uninstallHook();
		return;
	}


	g_hookAddr = uKiFastCallEntry + i;
	g_hookAddr_next_ins = g_hookAddr + 5;

	// 3. ��ʼinline hook
	UCHAR jmpCode[5] = { 0xe9 };// jmp xxxx
	disablePageWriteProtect();

	// 3.1 ������תƫ��
	// ��תƫ�� = Ŀ���ַ - ��ǰ��ַ - 5
	*(ULONG*)(jmpCode + 1) = (ULONG)myKiFastEntryHook - g_hookAddr - 5;

	// 3.2 ����תָ��д��
	RtlCopyMemory(uKiFastCallEntry + i,
		jmpCode,
		5);

	enablePageWriteProtect();
}


void ReloadKernel()
{
	isReload = 1;
	// 1. �ҵ��ں��ļ�·��
// 1.1 ͨ�������ں�����ķ�ʽ���ҵ��ں���ģ��
	LDR_DATA_TABLE_ENTRY* pLdr = ((LDR_DATA_TABLE_ENTRY*)g_DriverObg->DriverSection);
	// 1.2 �ں���ģ���������еĵ�2��.
	for (int i = 0; i < 2; ++i)
		pLdr = (LDR_DATA_TABLE_ENTRY*)pLdr->InLoadOrderLinks.Flink;

	g_ntKernelSize = pLdr->SizeOfImage;

	// 1.3 ���浱ǰ���ػ�ַ
	char* pCurKernelBase = (char*)pLdr->DllBase;

	KdPrint(("base=%p name=%wZ\n", pCurKernelBase, &pLdr->FullDllName));

	// 2. ��ȡntģ����ļ����ݵ��ѿռ�.
	loadNtKernelModule(&pLdr->FullDllName, &g_pNewNtKernel);

	// 3. �޸���ntģ����ض�λ.
	fixRelocation(g_pNewNtKernel, (ULONG)pCurKernelBase);

	// 4. ʹ�õ�ǰ����ʹ�õ��ں˵����������
	//    ���ں˵�SSDT��.
	initSSDT(g_pNewNtKernel, pCurKernelBase);

	// 5. HOOK KiFastCallEntry,ʹ���ú������ں˵�·��
	installHook();

}
//�����߳�
PETHREAD LookupThread(HANDLE hTid)
{
	PETHREAD pEThread = NULL;
	if (STATUS_SUCCESS == PsLookupThreadByThreadId((HANDLE)hTid, &pEThread))
	{
		return pEThread;
	}
	return NULL;
}
////////////////////////////////////////////
/////////////  sysenter hook��غ���   /////////////
////////////////////////////////////////////
// ж��HOOK
void uninstallSysenterHook()
{
	_asm
	{
		push edx;
		push eax;
		push ecx;
		;// ���µĺ������ý�ȥ.
		mov eax, [g_oldKiFastCallEntery];
		xor edx, edx;
		mov ecx, 0x176;
		wrmsr; // ָ��ʹ��ecx�Ĵ�����ֵ��ΪMSR�Ĵ�����ı��,��edx:eaxд�뵽�����ŵļĴ�����.
		pop ecx;
		pop eax;
		pop edx;
	}
}

void _declspec(naked) MyKiFastCallEntry()
{
	_asm
	{
		;// 1. �����ú�
		cmp eax, 0xBE;
		jne _DONE; // ���úŲ���0xBE,ִ�е�4��

		;// 2. ������ID�ǲ���Ҫ�����Ľ��̵�ID
		push eax; // ���ݼĴ���

		;// 2. ��ȡ����(����ID)
		mov eax, [edx + 0x14];// eax�������PCLIENT_ID
		mov eax, [eax];// eax�������PCLIENT_ID->UniqueProcess

		;// 3. �ж��ǲ���Ҫ�����Ľ���ID
		cmp eax, [g_uPid];
		pop eax;// �ָ��Ĵ���
		jne _DONE;// ����Ҫ�����Ľ��̾���ת

		;// 3.1 �ǵĻ��͸ĵ��ò���,�ú�����������ʧ��.
		mov dword ptr[edx + 0xC], 0; // ������Ȩ������Ϊ0

	_DONE:
		; // 4. ����ԭ����KiFastCallEntry
		jmp g_oldKiFastCallEntery;
	}
}
void _declspec(naked) installSysenterHook()
{
	_asm
	{
		push edx;
		push eax;
		push ecx;

		;// ����ԭʼ����
		mov ecx, 0x176;//SYSENTER_EIP_MSR�Ĵ����ı��.������KiFastCallEntry�ĵ�ַ
		rdmsr; // ָ��ʹ��ecx�Ĵ�����ֵ��ΪMSR�Ĵ�����ı��,�������ŵļĴ����е�ֵ��ȡ��edx:eax
		mov[g_oldKiFastCallEntery], eax;// ����ַ���浽ȫ�ֱ�����.

		;// ���µĺ������ý�ȥ.
		mov eax, MyKiFastCallEntry;
		xor edx, edx;
		wrmsr; // ָ��ʹ��ecx�Ĵ�����ֵ��ΪMSR�Ĵ�����ı��,��edx:eaxд�뵽�����ŵļĴ�����.
		pop ecx;
		pop eax;
		pop edx;
		ret;
	}
}


void  Unload(DRIVER_OBJECT* driver) {
	// ɾ����������
	UNICODE_STRING DosSymName;
	if (isSysHook == 1)
	{
		uninstallSysenterHook();
	}
	if (isReload == 1)
	{
		uninstallHook();

	}
	RtlInitUnicodeString(&DosSymName, L"\\DosDevices\\device_mARK");
	IoDeleteSymbolicLink(&DosSymName);
	// ж���豸����
	IoDeleteDevice(driver->DeviceObject);
}

NTSTATUS  Create(DEVICE_OBJECT* device, IRP* irp) {
	device;
	KdPrint(("������������\n"));
	// ����IRP���״̬
	irp->IoStatus.Status = STATUS_SUCCESS;
	irp->IoStatus.Information = 0;
	IoCompleteRequest(irp, IO_NO_INCREMENT);
	return STATUS_SUCCESS;
}
NTSTATUS  Close(DEVICE_OBJECT* device, IRP* irp) {

	device;
	KdPrint(("�������ر���\n"));
	// ����IRP���״̬
	irp->IoStatus.Status = STATUS_SUCCESS;
	irp->IoStatus.Information = 0;
	IoCompleteRequest(irp, IO_NO_INCREMENT);
	return STATUS_SUCCESS;
}


NTSTATUS  DeviceCtrl(DEVICE_OBJECT* device, IRP* irp)
{
	//_asm int 3;
	device;
	NTSTATUS status = STATUS_SUCCESS;
	ULONG	 complateSize = 0;
	HANDLE		hFile = NULL;

	// ����IRP���״̬
	irp->IoStatus.Status = STATUS_SUCCESS;

	// 1. ��ȡ����IOջ
	IO_STACK_LOCATION* ioStack = IoGetCurrentIrpStackLocation(irp);

	// 2. ��ȡ����Ĳ���:
	// 2.1 IO�豸������
	ULONG ctrlCode = ioStack->Parameters.DeviceIoControl.IoControlCode;

	// 2.2 ���뻺�������ֽ���
	ULONG inputSize = ioStack->Parameters.DeviceIoControl.InputBufferLength;

	// 2.3 ��ȡ������������ֽ���
	ULONG outputSize = ioStack->Parameters.DeviceIoControl.OutputBufferLength;

	// 2.4 ��ȡ���뻺����
	// �����ڿ�������ָ����ʲô���䷽ʽ,��ʹ��irp->AssociatedIrp.SystemBuffer
	// ���������뻺����������
	PVOID pInputBuff = irp->AssociatedIrp.SystemBuffer;

	// 2.5 ��ȡ���������
	// ���������ָ����`METHOD_OUT_DIRECT`, ��ô�������������MDL����ʽ
	// ����, ����ڵ���DeviceIoContral�����������������NULLʱ,`MdlAddress`
	// Ҳ��ΪNULL
	PVOID pOutBuff = NULL;


	if (irp->MdlAddress && METHOD_FROM_CTL_CODE(ctrlCode) & METHOD_OUT_DIRECT) {
		pOutBuff = MmGetSystemAddressForMdlSafe(irp->MdlAddress, 0);
	}


	// 2.6 ���ݿ����������в���
	switch (ctrlCode) // ���ݹ��ܺ���ִ�в�ͬ����.
	{
		NTSTATUS			Status = STATUS_SUCCESS;
	case DF_GET_FILE_FIRST: {
			//_asm int 3;
			HANDLE hFind = NULL;
			// �ȶ���һ��������,���ڱ���������ļ�����.
			char tempBuff[sizeof(FILE_BOTH_DIR_INFORMATION) + 267 * 2];
			FILE_BOTH_DIR_INFORMATION* pInfo = (FILE_BOTH_DIR_INFORMATION*)tempBuff;
			// ��ȡ��һ���ļ�
			status = firstFile((wchar_t*)pInputBuff, &hFind, pInfo, sizeof(tempBuff));

			if (status == STATUS_SUCCESS)
			{
				// ����Ϣ���

				if (pOutBuff != NULL && outputSize >= sizeof(FIND_FILE_INFO))
				{
					FIND_FILE_INFO* pFindInfo = (FIND_FILE_INFO*)pOutBuff;
					pFindInfo->hFindHandle = hFind;

					// ������Ϣ.
					RtlCopyMemory(pFindInfo->cFileName, pInfo->FileName, pInfo->FileNameLength);
					pFindInfo->cFileName[pInfo->FileNameLength / 2] = 0;
					pFindInfo->dwFileAttributes = pInfo->FileAttributes;
					pFindInfo->createTimeHigh = pInfo->CreationTime.HighPart;
					pFindInfo->createTimeLow = pInfo->CreationTime.LowPart;
					
					pFindInfo->ftLastAccessTime = pInfo->LastAccessTime.QuadPart;
					pFindInfo->writeTimeHigh = pInfo->LastWriteTime.HighPart;
					pFindInfo->writeTimeLow = pInfo->LastWriteTime.LowPart;
					pFindInfo->nFileSizeHigh = pInfo->EndOfFile.HighPart;
					pFindInfo->nFileSizeLow = pInfo->EndOfFile.LowPart;
					/*if (pOutBuff != NULL && outputSize >= sizeof(FIND_FILE_INFO))
					{
						RtlMoveMemory(pOutBuff, pFindInfo, sizeof(FIND_FILE_INFO));
					}*/
					// ������ɵ��ֽ���
					complateSize = sizeof(FIND_FILE_INFO);
				}
			}
		}break;
	case DF_GET_FILE_NEXT: {
			//_asm int 3;
			// ������뻺������������������ǽṹ��FIND_FILE_INFO,���˳�
			if (inputSize != sizeof(FIND_FILE_INFO) || outputSize != inputSize)
				break;

			// ��ȡ���뻺����
			FIND_FILE_INFO* pFindInfo = (FIND_FILE_INFO*)pOutBuff;

			// �ȶ���һ��������,���ڱ���������ļ�����.
			char tempBuff[sizeof(FILE_BOTH_DIR_INFORMATION) + 267 * 2];
			FILE_BOTH_DIR_INFORMATION* pInfo = (FILE_BOTH_DIR_INFORMATION*)tempBuff;
			// ��ȡ��һ���ļ�
			status = nextFile(pFindInfo->hFindHandle, pInfo, sizeof(tempBuff));
			if (status == STATUS_SUCCESS)
			{
				RtlCopyMemory(pFindInfo->cFileName, pInfo->FileName, pInfo->FileNameLength);
				pFindInfo->cFileName[pInfo->FileNameLength / 2] = 0;
				pFindInfo->dwFileAttributes = pInfo->FileAttributes;
				pFindInfo->createTimeHigh = pInfo->CreationTime.HighPart;
				pFindInfo->createTimeLow = pInfo->CreationTime.LowPart;

				pFindInfo->ftLastAccessTime = pInfo->LastAccessTime.QuadPart;
				
				pFindInfo->writeTimeHigh = pInfo->LastWriteTime.HighPart;
				pFindInfo->writeTimeLow = pInfo->LastWriteTime.LowPart;

				pFindInfo->nFileSizeHigh = pInfo->EndOfFile.HighPart;
				pFindInfo->nFileSizeLow = pInfo->EndOfFile.LowPart;
				/*if (pOutBuff != NULL && outputSize >= sizeof(FIND_FILE_INFO))
				{
					RtlMoveMemory(pOutBuff, pFindInfo, sizeof(FIND_FILE_INFO));
				}*/
				// ������ɵ��ֽ���
				complateSize = sizeof(FIND_FILE_INFO);
			}
			else
			{
				ZwClose(pFindInfo->hFindHandle);
			}

		}break;
	case DF_DELETE_FILE:
	{
		//_asm int 3;
		DbgPrint("delete file\n");
		if (pInputBuff)
		{
			__try
			{
				 
				status = removeFile(pInputBuff);
				irp->IoStatus.Status = status;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				DbgPrint("Catch Exception\r\n");
				status = STATUS_UNSUCCESSFUL;
			}
		}
	}break;
	case DF_GET_PROCESSNUM:
	{
	
		i = 0;//�����̸�������
		DbgPrint("Get Process Count\r\n");
		if (pOutBuff)
		{
			__try
			{

				DWORD ProcessCount = 0;
				ProcessCount=(DWORD)APGetProcessNum();
				//DbgPrint("%d\n", ProcessCount);
				RtlMoveMemory(pOutBuff, &ProcessCount, sizeof(DWORD));
				//DbgPrint("%d\n", *(DWORD*)pOutBuff);
				irp->IoStatus.Status = status;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				DbgPrint("Catch Exception\r\n");
				status = STATUS_UNSUCCESSFUL;
			}
		}
		else
		{
			irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
		}


	}break;
	case DF_ENUM_PROCESS:
	{
		//_asm int 3;
		//DbgPrint("Enum Process\n");
		 if (pOutBuff)
		{
			__try
			{
				PEPROCESS proc = NULL;
				PROCESS_ENTRY_INFO fpi = { 0 };
				
				while ( i <= 0x10000)
				{
					i += sizeof(HANDLE);
					if (STATUS_SUCCESS == PsLookupProcessByProcessId((HANDLE)i, &proc))
					{
						fpi.EProcess = proc;
						fpi.ProcessId = i;
						fpi.ParentProcessId = *(ULONG_PTR*)((ULONG_PTR)proc + 0x140);//WIN64��0x290
						RtlCopyMemory(fpi.szImageName, PsGetProcessImageFileName(proc), 100);
						GetProcessImagePath((ULONG)i, fpi.wzFilePath);//ZwQueryInformationProcess
						if (pOutBuff != NULL && outputSize >= sizeof(PROCESS_ENTRY_INFO))
						{
							RtlMoveMemory(pOutBuff, &fpi, sizeof(PROCESS_ENTRY_INFO));
						}
						// �ݼ����ü���
						ObDereferenceObject(proc);
						break;
					}
					
					irp->IoStatus.Status = status;
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				DbgPrint("Catch Exception\r\n");
				status = STATUS_UNSUCCESSFUL;
			}
		}
		else
		{
			irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
		}
	
	}break;
	case DF_PROTECT_PRPCESS:
	{
		//_asm int 3;
		//DbgPrint("protect process\n");
		if (pInputBuff)
		{
			__try
			{
				isSysHook = 1;
				RtlMoveMemory(&g_uPid, pInputBuff, sizeof(ULONG));

				installSysenterHook();
				irp->IoStatus.Status = status;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				DbgPrint("Catch Exception\r\n");
				status = STATUS_UNSUCCESSFUL;
			}
		}
	}break;
	case DF_TERMINATE_PRPCESS:
	{
		//_asm int 3;
		DbgPrint("terminate process\n");
		if (pInputBuff)
		{
			__try
			{
				ULONG pid = 0;
				//g_uPid = *(ULONG*)pInputBuff;
				RtlMoveMemory(&pid, pInputBuff, sizeof(ULONG));

				HANDLE hProcess = NULL;
				CLIENT_ID ClientId = { 0 };
				OBJECT_ATTRIBUTES objAttribute = { sizeof(OBJECT_ATTRIBUTES) };
				ClientId.UniqueProcess = (HANDLE)pid;
				ClientId.UniqueThread = 0;
				ZwOpenProcess(&hProcess, 1, &objAttribute, &ClientId);
				if (hProcess)
				{
					ZwTerminateProcess(hProcess, 0);
					ZwClose(hProcess);
				}

				irp->IoStatus.Status = status;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				DbgPrint("Catch Exception\r\n");
				status = STATUS_UNSUCCESSFUL;
			}
		}
	}break;
	case DF_ENUM_MODULEFIRST:
	{
		//_asm int 3;
		DWORD modulenum = 0;
		
		if (pInputBuff)
		{
			__try
			{
				
				//g_uPid = *(ULONG*)pInputBuff;
				RtlMoveMemory(&modulePID, pInputBuff, sizeof(ULONG));

				PEPROCESS proc = NULL;
				if (NT_SUCCESS(PsLookupProcessByProcessId((HANDLE)modulePID, &proc)))
				{
					// ����ģ��
					// 1. �ҵ�PEB(����PEB���û���ռ�,�����Ҫ���̹ҿ�
					KAPC_STATE kapc_status = { 0 };
					KeStackAttachProcess(proc, &kapc_status);
					// 2. �ҵ�PEB.Ldr(ģ������)
					peb = PsGetProcessPeb(proc);
					if (peb != NULL)
					{
						__try {
							// 3. ����ģ������
							pLdrEntry = (LDR_DATA_TABLE_ENTRY*)peb->Ldr->InLoadOrderModuleList.Flink;

							LDR_DATA_TABLE_ENTRY* pLdrEntryy = (LDR_DATA_TABLE_ENTRY*)peb->Ldr->InLoadOrderModuleList.Flink;
							LDR_DATA_TABLE_ENTRY* pBegin = pLdrEntryy;
							do
							{
								modulenum++;
								/*KdPrint(("\tBASE:%p SIZE:%06X %wZ\n",
									pLdrEntry->DllBase,
									pLdrEntry->SizeOfImage,
									&pLdrEntry->FullDllName));*/

								// �ҵ���һ��
								pLdrEntryy = (LDR_DATA_TABLE_ENTRY*)pLdrEntryy->InLoadOrderLinks.Flink;
							} while (pBegin != pLdrEntryy);
						}
						__except (EXCEPTION_EXECUTE_HANDLER) {}
					}

					// ����ҿ�
					KeUnstackDetachProcess(&kapc_status);
					// �ݼ����ü���
					ObDereferenceObject(proc);
				}
				RtlMoveMemory(pOutBuff, &modulenum, sizeof(DWORD));

				irp->IoStatus.Status = status;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				DbgPrint("Catch Exception\r\n");
				status = STATUS_UNSUCCESSFUL;
			}
		}
	}break;
	case DF_ENUM_MODULENEXT:
	{
		//__asm int 3;
		DbgPrint("Enum Module\n");
		DbgPrint("terminate process\n");
		if (pOutBuff)
		{
			__try
			{
				PEPROCESS proc = NULL;
				if (NT_SUCCESS(PsLookupProcessByProcessId((HANDLE)modulePID, &proc)))
				{
					KAPC_STATE kapc_status = { 0 };
					KeStackAttachProcess(proc, &kapc_status);
					if (peb != NULL)
					{
						__try {
							// 3. ����ģ������
							//pLdrEntry = (LDR_DATA_TABLE_ENTRY*)peb->Ldr->InLoadOrderModuleList.Flink;
							MODULE_ENTRY temp = { 0 };
							temp.BaseAddress = pLdrEntry->DllBase;
							temp.SizeOfImage = pLdrEntry->SizeOfImage;
							KdPrint(("%wZ\n", &pLdrEntry->FullDllName));
							RtlCopyMemory(temp.wzFilePath, pLdrEntry->FullDllName.Buffer, pLdrEntry->FullDllName.Length);
							temp.wzFilePath[pLdrEntry->FullDllName.Length / 2] = 0;
							RtlCopyMemory(pOutBuff, &temp, sizeof(MODULE_ENTRY));

							pLdrEntry = (LDR_DATA_TABLE_ENTRY*)pLdrEntry->InLoadOrderLinks.Flink;
						}
						__except (EXCEPTION_EXECUTE_HANDLER) {
							DbgPrint("Catch Exception\r\n");
							status = STATUS_UNSUCCESSFUL;
						}
					}
					// ����ҿ�
					KeUnstackDetachProcess(&kapc_status);
					// �ݼ����ü���
					ObDereferenceObject(proc);
				}

				irp->IoStatus.Status = status;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				DbgPrint("Catch Exception\r\n");
				status = STATUS_UNSUCCESSFUL;
			}
		}
	}break;
	case DF_ENUM_THREADFIRST:
	{
		
		DWORD threadnum = 0;
		ithread = 0;
		if (pInputBuff)
		{
			__try
			{

				RtlMoveMemory(&threadPeprocess, pInputBuff, sizeof(ULONG));

				PEPROCESS pEProc = NULL;
				PETHREAD pEThrd = NULL;
				ULONG i = 0;
				for (i = 4; i < 0x25600; i += 4)
				{
					pEThrd = LookupThread((HANDLE)i);
					if (!pEThrd)
					{
						continue;
					}
					pEProc = IoThreadToProcess(pEThrd);
					if (pEProc == threadPeprocess)
					{
						threadnum++;
					}
					ObDereferenceObject(pEThrd);
				}
				RtlMoveMemory(pOutBuff, &threadnum, sizeof(DWORD));

				irp->IoStatus.Status = status;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				DbgPrint("Catch Exception\r\n");
				status = STATUS_UNSUCCESSFUL;
			}
		}
	}break;
	case DF_ENUM_THREADNEXT:
	{
		//_asm int 3;
		if (pOutBuff)
		{
			__try
			{

				PEPROCESS pEProc = NULL;
				PETHREAD pEThrd = NULL;
				THREAD_ENTRY temp = { 0 };
				while ( ithread < 0x25600)
				{
					ithread += 4;
					pEThrd = LookupThread((HANDLE)ithread);
					if (!pEThrd)
					{
						continue;
					}
					pEProc = IoThreadToProcess(pEThrd);
					if (pEProc == threadPeprocess)
					{
						temp.Ethread = pEThrd;
						temp.TID = (ULONG)PsGetThreadId(pEThrd);
						break;
					}
					ObDereferenceObject(pEThrd);
				}
				RtlMoveMemory(pOutBuff, &temp, sizeof(THREAD_ENTRY));

				irp->IoStatus.Status = status;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				DbgPrint("Catch Exception\r\n");
				status = STATUS_UNSUCCESSFUL;
			}
		}
	}break;
	case DF_GET_DRIVERNUM:
	{
		

		driverNum = 0;
		pLdr = (PLDR_DATA_TABLE_ENTRY)g_DriverObg->DriverSection;
		pTemp = &pLdr->InLoadOrderLinks;
		__try
		{
			PLDR_DATA_TABLE_ENTRY pLdrr = (PLDR_DATA_TABLE_ENTRY)g_DriverObg->DriverSection;
			LIST_ENTRY* pTempp = &pLdrr->InLoadOrderLinks;
			do
			{
				driverNum++;
				pTempp = pTempp->Blink;
			} while (pTempp != &pLdrr->InLoadOrderLinks);
			RtlMoveMemory(pOutBuff, &driverNum, sizeof(DWORD));
			irp->IoStatus.Status = status;
		}
		
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			DbgPrint("Catch Exception\r\n");
			status = STATUS_UNSUCCESSFUL;
		}
	}break;
	case DF_ENUM_DRIVER:
	{
		if (pOutBuff)
		{
			__try
			{
				{
					PLDR_DATA_TABLE_ENTRY pDriverInfo = (PLDR_DATA_TABLE_ENTRY)pTemp;
					DRIVER_ENTRY_INFO temp = { 0 };
					temp.BaseAddress = pDriverInfo->DllBase;
				
					temp.Size = pDriverInfo->SizeOfImage;
					RtlStringCchCopyW(temp.wzDriverPath, pDriverInfo->FullDllName.Length / sizeof(WCHAR) + 1,
						(WCHAR*)pDriverInfo->FullDllName.Buffer);
					RtlStringCchCopyW(temp.wzDriverName, pDriverInfo->BaseDllName.Length / sizeof(WCHAR) + 1,
						(WCHAR*)pDriverInfo->BaseDllName.Buffer);
				
					if (pOutBuff != NULL && outputSize >= sizeof(DRIVER_ENTRY_INFO))
					{
						RtlMoveMemory(pOutBuff, &temp, sizeof(DRIVER_ENTRY_INFO));
					}

					pTemp = pTemp->Blink;
				}
				irp->IoStatus.Status = STATUS_SUCCESS;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				DbgPrint("Catch Exception\r\n");
				Status = STATUS_UNSUCCESSFUL;
			}
		}
		else
		{
			irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
		}

	}break;
	case DF_HIDE_DRIVER:
	{
	

		if (pInputBuff)
		{
			__try
			{
				UNICODE_STRING driverName;
				RtlInitUnicodeString(&driverName, (PCWSTR)pInputBuff);
				DbgPrint("driver��%wZ\n", driverName.Buffer);

				HideDriver(g_DriverObg, &driverName);
				irp->IoStatus.Status = status;
			}

			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				DbgPrint("Catch Exception\r\n");
				status = STATUS_UNSUCCESSFUL;
			}
		}
		
	}break;
	case DF_GET_IDTFIRST:
	{
		idtnum = 0;
	}break;
	case DF_GET_IDT:
	{
		//_asm int 3;
		DbgPrint("Get IDT\n");
		
		if (pOutBuff)
		{
			__try
			{
				idtnum++;
				IDT_INFO SIDT = { 0 };
				PIDT_ENTRY pIDTEntry = NULL;
				ULONG uAddr = 0;
				//��ȡIDT��ĵ�ַ
				_asm sidt SIDT;
				//��ȡIDT�������ַ
				pIDTEntry = (PIDT_ENTRY)MAKELONG(SIDT.uLowIdtBase, SIDT.uHighIdtBase);
				//for (; idtnum < 0x100; idtnum++)
				{
					if (pOutBuff != NULL && outputSize >= sizeof(IDT_ENTRY))
					{
						RtlMoveMemory(pOutBuff, &pIDTEntry[idtnum], sizeof(IDT_ENTRY));
					}
				}
				irp->IoStatus.Status = status;
			}

			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				DbgPrint("Catch Exception\r\n");
				status = STATUS_UNSUCCESSFUL;
			}
		}
	}break;
	case DF_HIDE_PROCESS:
	{
		//_asm int 3;
		if (pInputBuff)
		{
			__try
			{
				ULONG pid = 0;
				RtlMoveMemory(&pid, pInputBuff, sizeof(ULONG));

				HideProcess(pid);
				irp->IoStatus.Status = status;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				DbgPrint("Catch Exception\r\n");
				status = STATUS_UNSUCCESSFUL;
			}
		}
	}break;
	case DF_GET_GDTFIRST:
	{
		gdtnum = 0;
	}break; 
	case DF_GET_GDT:
	{
		DbgPrint("Get GDT\n");

		if (pOutBuff)
		{
			__try
			{
				gdtnum++;
				GDT_INFO SGDT = { 0 };
				PGDT_ENTRY pGDTEntry = NULL;
				ULONG uAddr = 0;
				//��ȡGDT��ĵ�ַ
				_asm sgdt SGDT;
				//��ȡGDT�������ַ
				pGDTEntry = (PGDT_ENTRY)MAKELONG(SGDT.uLowGdtBase, SGDT.uHighGdtBase);
				//for (; idtnum < 0x100; idtnum++)
				{
					if (pOutBuff != NULL && outputSize >= sizeof(GDT_ENTRY))
					{
						RtlMoveMemory(pOutBuff, &pGDTEntry[gdtnum], sizeof(GDT_ENTRY));
					}
				}
				irp->IoStatus.Status = status;
			}

			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				DbgPrint("Catch Exception\r\n");
				status = STATUS_UNSUCCESSFUL;
			}
		}
	}break;
	case DF_GET_SSDT_COUNT:
	{
		ssdtnum = 0;
		__try
		{

			DWORD ssdtn = KeServiceDescriptorTable->NumberOfService;
			RtlMoveMemory(pOutBuff, &ssdtn, sizeof(DWORD));
			irp->IoStatus.Status = status;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			DbgPrint("Catch Exception\r\n");
			status = STATUS_UNSUCCESSFUL;
		}
	}break;
	case DF_ENUM_SSDT:
	{
		DbgPrint("Enum SsdtHook\r\n");
		//_asm int 3;
		if (pOutBuff)
		{
			__try
			{
				LONG addr = GetFunticonAddr(KeServiceDescriptorTable,ssdtnum);
				//DbgPrint("address[%d]:0x%08X\n", ssdtnum,KeServiceDescriptorTable->ServiceTableBase + ssdtnum * 4);
				RtlMoveMemory(pOutBuff, &addr, sizeof(LONG));
				
				ssdtnum++;

				irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				DbgPrint("Catch Exception\r\n");
				Status = STATUS_UNSUCCESSFUL;
			}
		}
		else
		{
			irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
		}
	}break;
	case DF_KERNEL_RELOAD:
	{
		_asm int 3;
		__try
		{
			ReloadKernel();

			irp->IoStatus.Status = STATUS_SUCCESS;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			DbgPrint("Catch Exception\r\n");
			Status = STATUS_UNSUCCESSFUL;
		}
	}break;
	}
	irp->IoStatus.Information = complateSize;
	IoCompleteRequest(irp, IO_NO_INCREMENT);

	return status;
}

NTSTATUS DriverEntry(DRIVER_OBJECT* driver, UNICODE_STRING* path)
{
	path;
	_asm int 3;
	NTSTATUS status = STATUS_SUCCESS;
	UNICODE_STRING NtDevName;
	RtlInitUnicodeString(&NtDevName, L"\\Device\\mydevice");

	// 1. ����һ���豸����.�����޷��󶨷�������,û�з�������, �û�������Ӳ����ں˲�
	DEVICE_OBJECT* dev = NULL;
	status = IoCreateDevice(driver,
		0,
		&NtDevName,
		FILE_DEVICE_UNKNOWN,
		0,
		0,
		&dev);

	dev->Flags |= DO_DIRECT_IO; // ʹ��ֱ��IO

	if (status != STATUS_SUCCESS)
		return status;

	UNICODE_STRING DosSymName;
	RtlInitUnicodeString(&DosSymName, L"\\DosDevices\\device_mARK");
	//2. Ϊ�豸����󶨷�������
	status = IoCreateSymbolicLink(&DosSymName, &NtDevName);
	if (NT_SUCCESS(status))
	{
		DbgPrint("mARK is Starting!!!\n");
	}
	//��ж�غ���
	driver->DriverUnload = Unload;
	g_DriverObg = driver;
	/*isReload = 1;
	ReloadKernel();*/


	// ����ǲ����, �����������������, �û����ڴ�����ʱ��ʧ��.
	driver->MajorFunction[IRP_MJ_CREATE] = Create;
	driver->MajorFunction[IRP_MJ_CLOSE] = Close;
	driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DeviceCtrl;

	return status;

}
