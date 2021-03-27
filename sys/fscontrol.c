/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2017 - 2021 Google, Inc.
  Copyright (C) 2015 - 2019 Adrien J. <liryna.stark@gmail.com> and Maxime C. <maxime@islog.com>
  Copyright (C) 2007 - 2011 Hiroki Asakawa <info@dokan-dev.net>

  http://dokan-dev.github.io

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the Free
Software Foundation; either version 3 of the License, or (at your option) any
later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU Lesser General Public License along
with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "dokan.h"
#include "util/fcb.h"
#include "util/mountmgr.h"
#include "util/irp_buffer_helper.h"
#include "util/str.h"

#include <wdmsec.h>

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, DokanOplockRequest)
#endif
#include <mountdev.h>

void DokanMaybeLogOplockRequest(__in PDOKAN_LOGGER Logger,
                                __in PDokanFCB Fcb,
                                __in ULONG FsControlCode,
                                __in ULONG OplockCount,
                                __in BOOLEAN AcquiredFcb,
                                __in BOOLEAN AcquiredVcb,
                                __in ULONG RequestedLevel,
                                __in ULONG Flags) {
  // These calls log to the fixed-size DokanOlockDebugInfo and not to a file, so
  // we enable them no matter what.
  OplockDebugRecordRequest(Fcb, FsControlCode, RequestedLevel);
  if (Flags & REQUEST_OPLOCK_INPUT_FLAG_ACK) {
    OplockDebugRecordFlag(Fcb, DOKAN_OPLOCK_DEBUG_GENERIC_ACKNOWLEDGEMENT);
  }
  // Don't log via the Event Log unless flagged on (which should never be the
  // case by default).
  if (!DokanOpLockDebugEnabled()) {
    return;
  }
  if (FsControlCode == FSCTL_REQUEST_OPLOCK) {
    DokanLogInfo(Logger, L"Oplock request FSCTL_REQUEST_OPLOCK for file \"%wZ\";"
                 L" oplock count %d; acquired FCB %d; acquired VCB %d;"
                 L" level = %I32x; flags = %I32x",
                 &Fcb->FileName, OplockCount, AcquiredFcb, AcquiredVcb,
                 RequestedLevel, Flags);
    return;
  }
  DokanLogInfo(Logger, L"Oplock request %s for file \"%wZ\"; oplock count %d;"
               L" acquired FCB %d; acquired VCB %d",
               DokanGetIoctlStr(FsControlCode),
               &Fcb->FileName, OplockCount, AcquiredFcb, AcquiredVcb);
}

void DokanMaybeLogOplockResult(__in PDOKAN_LOGGER Logger,
                               __in PDokanFCB Fcb,
                               __in ULONG FsControlCode,
                               __in ULONG RequestedLevel,
                               __in ULONG Flags,
                               __in NTSTATUS Status) {
  if (!DokanOpLockDebugEnabled()) {
    return;
  }
  if (FsControlCode == FSCTL_REQUEST_OPLOCK) {
    DokanLogInfo(Logger, L"Oplock result for FSCTL_REQUEST_OPLOCK for file \"%wZ\";"
                 L" level = %I32x; flags = %I32x; status = 0x%I32x",
                 &Fcb->FileName, RequestedLevel, Flags, Status);
    return;
  }
  DokanLogInfo(Logger, L"Oplock result for %s for file \"%wZ\"; status = 0x%I32x",
               DokanGetIoctlStr(FsControlCode), &Fcb->FileName,
               Status);
}

NTSTATUS DokanOplockRequest(__in PIRP *pIrp) {
  NTSTATUS status = STATUS_SUCCESS;
  ULONG fsControlCode;
  PDokanDCB dcb;
  PDokanVCB vcb;
  PDokanFCB fcb = NULL;
  PDokanCCB ccb;
  PFILE_OBJECT fileObject;
  PIRP irp = *pIrp;
  ULONG oplockCount = 0;

  PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(irp);

  BOOLEAN acquiredVcb = FALSE;
  BOOLEAN acquiredFcb = FALSE;

  PREQUEST_OPLOCK_INPUT_BUFFER inputBuffer = NULL;
  ULONG outputBufferLength;

  PAGED_CODE();

  //
  //  Save some references to make our life a little easier
  //
  fsControlCode = irpSp->Parameters.FileSystemControl.FsControlCode;

  fileObject = irpSp->FileObject;
  DOKAN_LOG_FINE_IRP(irp, "FileObject=%p", fileObject);

  ccb = fileObject->FsContext2;
  if (ccb == NULL || ccb->Identifier.Type != CCB) {
    DOKAN_LOG_FINE_IRP(irp, "Invalid CCB or wrong type");
    return STATUS_INVALID_PARAMETER;
  }

  fcb = ccb->Fcb;
  if (fcb == NULL || fcb->Identifier.Type != FCB) {
    DOKAN_LOG_FINE_IRP(irp, "Invalid FCB or wrong type");
    return STATUS_INVALID_PARAMETER;
  }
  OplockDebugRecordMajorFunction(fcb, IRP_MJ_FILE_SYSTEM_CONTROL);
  vcb = fcb->Vcb;
  if (vcb == NULL || vcb->Identifier.Type != VCB) {
    DOKAN_LOG_FINE_IRP(irp, "Invalid Vcb or wrong type");
    return STATUS_INVALID_PARAMETER;
  }
  DOKAN_INIT_LOGGER(logger, vcb->DeviceObject->DriverObject, 0);

  dcb = vcb->Dcb;
  if (dcb == NULL || dcb->Identifier.Type != DCB) {
    return STATUS_INVALID_PARAMETER;
  }

  //
  //  Get the input & output buffer lengths and pointers.
  //
  if (fsControlCode == FSCTL_REQUEST_OPLOCK) {

    outputBufferLength = irpSp->Parameters.FileSystemControl.OutputBufferLength;

    //
    //  Check for a minimum length on the input and ouput buffers.
    //
    GET_IRP_BUFFER_OR_RETURN(irp, inputBuffer)
    // Use OutputBuffer only for buffer size check
    if (outputBufferLength < sizeof(REQUEST_OPLOCK_OUTPUT_BUFFER)) {
      return STATUS_BUFFER_TOO_SMALL;
    }
  }

  //
  //  If the oplock request is on a directory it must be for a Read or
  //  Read-Handle oplock only.
  //
  if ((DokanFCBFlagsIsSet(fcb, DOKAN_FILE_DIRECTORY)) &&
      ((fsControlCode != FSCTL_REQUEST_OPLOCK) ||
       !FsRtlOplockIsSharedRequest(irp))) {

    DOKAN_LOG_FINE_IRP(irp, "Only read oplock allowed for directories");
    return STATUS_INVALID_PARAMETER;
  }

  //
  //  Use a try finally to free the Fcb/Vcb
  //
  try {

    //
    //  We grab the Fcb exclusively for oplock requests, shared for oplock
    //  break acknowledgement.
    //
    if ((fsControlCode == FSCTL_REQUEST_OPLOCK_LEVEL_1) ||
        (fsControlCode == FSCTL_REQUEST_BATCH_OPLOCK) ||
        (fsControlCode == FSCTL_REQUEST_FILTER_OPLOCK) ||
        (fsControlCode == FSCTL_REQUEST_OPLOCK_LEVEL_2) ||
        ((fsControlCode == FSCTL_REQUEST_OPLOCK) &&
            FlagOn(inputBuffer->Flags, REQUEST_OPLOCK_INPUT_FLAG_REQUEST))
    ) {

      DokanVCBLockRO(fcb->Vcb);
      acquiredVcb = TRUE;
      DokanFCBLockRW(fcb);
      acquiredFcb = TRUE;

      if (!(dcb->MountOptions & DOKAN_EVENT_FILELOCK_USER_MODE)) {

        if (FsRtlOplockIsSharedRequest(irp)) {
          //
          //  Byte-range locks are only valid on files.
          //
          if (!DokanFCBFlagsIsSet(fcb, DOKAN_FILE_DIRECTORY)) {

            //
            //  Set OplockCount to nonzero if FsRtl denies access
            //  based on current byte-range lock state.
            //
            if (DokanFsRtlCheckLockForOplockRequest) // Win8+
              oplockCount = (ULONG)!DokanFsRtlCheckLockForOplockRequest(
                  &fcb->FileLock, &fcb->AdvancedFCBHeader.AllocationSize);
            else
              oplockCount = (ULONG)FsRtlAreThereCurrentOrInProgressFileLocks(
                  &fcb->FileLock);
          }
        } else {
          // Shouldn't be something like UncleanCount counter and not FileCount
          // here?
          oplockCount = fcb->FileCount;
        }
      }
    } else if ((fsControlCode == FSCTL_OPLOCK_BREAK_ACKNOWLEDGE) ||
               (fsControlCode == FSCTL_OPBATCH_ACK_CLOSE_PENDING) ||
               (fsControlCode == FSCTL_OPLOCK_BREAK_NOTIFY) ||
               (fsControlCode == FSCTL_OPLOCK_BREAK_ACK_NO_2) ||
               ((fsControlCode == FSCTL_REQUEST_OPLOCK) &&
                   FlagOn(inputBuffer->Flags, REQUEST_OPLOCK_INPUT_FLAG_ACK))
    ) {
      DokanFCBLockRO(fcb);
      acquiredFcb = TRUE;
    } else if (fsControlCode == FSCTL_REQUEST_OPLOCK) {
      //
      //  The caller didn't provide either REQUEST_OPLOCK_INPUT_FLAG_REQUEST or
      //  REQUEST_OPLOCK_INPUT_FLAG_ACK on the input buffer.
      //
      status = STATUS_INVALID_PARAMETER;
      __leave;
    } else {
      status = STATUS_INVALID_PARAMETER;
      __leave;
    }

    //
    //  Fail batch, filter, and handle oplock requests if the file is marked
    //  for delete.
    //
    if (((fsControlCode == FSCTL_REQUEST_FILTER_OPLOCK) ||
         (fsControlCode == FSCTL_REQUEST_BATCH_OPLOCK) ||
         ((fsControlCode == FSCTL_REQUEST_OPLOCK) &&
          FlagOn(inputBuffer->RequestedOplockLevel, OPLOCK_LEVEL_CACHE_HANDLE))
             ) &&
        DokanFCBFlagsIsSet(fcb, DOKAN_DELETE_ON_CLOSE)) {
      status = STATUS_DELETE_PENDING;
      __leave;
    }

    ULONG level = 0;
    ULONG flags = 0;
    if (fsControlCode == FSCTL_REQUEST_OPLOCK) {
      level = inputBuffer->RequestedOplockLevel;
      flags = inputBuffer->Flags;
    }
    DokanMaybeLogOplockRequest(&logger, fcb, fsControlCode, oplockCount,
                               acquiredFcb, acquiredVcb, level, flags);

    //
    //  Call the FsRtl routine to grant/acknowledge oplock.
    //
    status = FsRtlOplockFsctrl(DokanGetFcbOplock(fcb), irp, oplockCount);
    DokanMaybeLogOplockResult(&logger, fcb, fsControlCode, level, flags,
                              status);
    //
    //  Once we call FsRtlOplockFsctrl, we no longer own the IRP and we should
    //  not complete it.
    //
    *pIrp = NULL;

  } finally {

    //
    //  Release all of our resources
    //
    if (acquiredFcb) {
      DokanFCBUnlock(fcb);
    }
    if (acquiredVcb) {
      DokanVCBUnlock(fcb->Vcb);
    }
  }

  return status;
}

NTSTATUS
DokanUserFsRequest(__in PDEVICE_OBJECT DeviceObject, __in PIRP *pIrp) {
  NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
  PIO_STACK_LOCATION irpSp;
  PFILE_OBJECT fileObject = NULL;
  PDokanCCB ccb = NULL;
  PDokanFCB fcb = NULL;
  DOKAN_INIT_LOGGER(logger, DeviceObject->DriverObject,
                    IRP_MJ_FILE_SYSTEM_CONTROL);

  irpSp = IoGetCurrentIrpStackLocation(*pIrp);
  DOKAN_LOG_IOCTL(*pIrp, irpSp->Parameters.FileSystemControl.FsControlCode,
                  "FileObject=%p", irpSp->FileObject)
  switch (irpSp->Parameters.FileSystemControl.FsControlCode) {
  case FSCTL_ACTIVATE_KEEPALIVE:
    fileObject = irpSp->FileObject;
    if (fileObject == NULL) {
      return DokanLogError(
          &logger,
          STATUS_INVALID_PARAMETER,
          L"Received FSCTL_ACTIVATE_KEEPALIVE with no FileObject.");
    }
    ccb = fileObject->FsContext2;
    if (ccb == NULL || ccb->Identifier.Type != CCB) {
      return DokanLogError(
          &logger,
          STATUS_INVALID_PARAMETER,
          L"Received FSCTL_ACTIVATE_KEEPALIVE with no CCB.");
    }

    fcb = ccb->Fcb;
    if (fcb == NULL || fcb->Identifier.Type != FCB) {
      return DokanLogError(
          &logger,
          STATUS_INVALID_PARAMETER,
          L"Received FSCTL_ACTIVATE_KEEPALIVE with no FCB.");
    }

    if (!fcb->IsKeepalive) {
      return DokanLogError(
          &logger,
          STATUS_INVALID_PARAMETER,
          L"Received FSCTL_ACTIVATE_KEEPALIVE for wrong file: \"%wZ\"",
          &fcb->FileName);
    }

    if (fcb->Vcb->IsKeepaliveActive && !ccb->IsKeepaliveActive) {
      return DokanLogError(
          &logger,
          STATUS_INVALID_PARAMETER,
          L"Received FSCTL_ACTIVATE_KEEPALIVE when a different keepalive handle"
          L" was already active.");
    }

    DokanLogInfo(&logger, L"Activating keepalive handle from process %lu.",
                 IoGetRequestorProcessId(*pIrp));
    DokanFCBLockRW(fcb);
    ccb->IsKeepaliveActive = TRUE;
    fcb->Vcb->IsKeepaliveActive = TRUE;
    DokanFCBUnlock(fcb);
    status = STATUS_SUCCESS;
    break;

  case FSCTL_NOTIFY_PATH: {
    PDOKAN_NOTIFY_PATH_INTERMEDIATE pNotifyPath = NULL;
    GET_IRP_NOTIFY_PATH_INTERMEDIATE_OR_RETURN(*pIrp, pNotifyPath)

    irpSp = IoGetCurrentIrpStackLocation(*pIrp);
    fileObject = irpSp->FileObject;
    if (fileObject == NULL) {
      return DokanLogError(
          &logger,
          STATUS_INVALID_PARAMETER,
          L"Received FSCTL_NOTIFY_PATH with no FileObject.");
    }
    ccb = fileObject->FsContext2;
    if (ccb == NULL || ccb->Identifier.Type != CCB) {
      return DokanLogError(
          &logger,
          STATUS_INVALID_PARAMETER,
          L"Received FSCTL_NOTIFY_PATH with no CCB.");
    }
    fcb = ccb->Fcb;
    if (fcb == NULL || fcb->Identifier.Type != FCB) {
      return DokanLogError(
          &logger,
          STATUS_INVALID_PARAMETER,
          L"Received FSCTL_NOTIFY_PATH with no FCB.");
    }
    UNICODE_STRING receivedBuffer;
    receivedBuffer.Length = pNotifyPath->Length;
    receivedBuffer.MaximumLength = pNotifyPath->Length;
    receivedBuffer.Buffer = pNotifyPath->Buffer;
    DOKAN_LOG_FINE_IRP(*pIrp,
                  "CompletionFilter: %lu, Action: %lu, "
                  "Length: %i, Path: \"%wZ\"",
                  pNotifyPath->CompletionFilter, pNotifyPath->Action,
                  receivedBuffer.Length, &receivedBuffer);
    DokanFCBLockRO(fcb);
    status = DokanNotifyReportChange0(
        fcb, &receivedBuffer, pNotifyPath->CompletionFilter,
        pNotifyPath->Action);
    DokanFCBUnlock(fcb);
    if (status == STATUS_OBJECT_NAME_INVALID) {
      DokanCleanupAllChangeNotificationWaiters(fcb->Vcb);
    }
    break;
  }

  case FSCTL_REQUEST_OPLOCK_LEVEL_1:
    status = DokanOplockRequest(pIrp);
    break;

  case FSCTL_REQUEST_OPLOCK_LEVEL_2:
    status = DokanOplockRequest(pIrp);
    break;

  case FSCTL_REQUEST_BATCH_OPLOCK:
    status = DokanOplockRequest(pIrp);
    break;

  case FSCTL_OPLOCK_BREAK_ACKNOWLEDGE:
    status = DokanOplockRequest(pIrp);
    break;

  case FSCTL_OPBATCH_ACK_CLOSE_PENDING:
    status = DokanOplockRequest(pIrp);
    break;

  case FSCTL_OPLOCK_BREAK_NOTIFY:
    status = DokanOplockRequest(pIrp);
    break;

  case FSCTL_OPLOCK_BREAK_ACK_NO_2:
    status = DokanOplockRequest(pIrp);
    break;

  case FSCTL_REQUEST_FILTER_OPLOCK:
    status = DokanOplockRequest(pIrp);
    break;

  case FSCTL_REQUEST_OPLOCK:
    status = DokanOplockRequest(pIrp);
    break;

  case FSCTL_LOCK_VOLUME:
  case FSCTL_UNLOCK_VOLUME:
  case FSCTL_IS_VOLUME_MOUNTED:
    status = STATUS_SUCCESS;
    break;

  case FSCTL_GET_REPARSE_POINT:
    status = STATUS_NOT_A_REPARSE_POINT;
    break;
  default:
    DOKAN_LOG_FINE_IRP(*pIrp, "Unsupported FsControlCode %x",
                       irpSp->Parameters.FileSystemControl.FsControlCode);
  }

  return status;
}

// Returns TRUE if |dcb| type matches |DCB| and FALSE otherwise.
BOOLEAN MatchDokanDCBType(__in PIRP Irp,
                          __in PDokanDCB Dcb,
                          __in PDOKAN_LOGGER Logger,
                          __in BOOLEAN LogFailures) {
  UNREFERENCED_PARAMETER(Irp);
  UNREFERENCED_PARAMETER(Logger);
  if (!Dcb) {
    if (LogFailures) {
      DOKAN_LOG_FINE_IRP(Irp, "There is no DCB.");
    }
    return FALSE;
  }
  if (GetIdentifierType(Dcb) != DCB) {
    if (LogFailures) {
      DOKAN_LOG_FINE_IRP(Irp, "The DCB type is actually %s expected %s.",
                    DokanGetIdTypeStr(Dcb), STR(DCB));
    }
    return FALSE;
  }
  return TRUE;
}

PCHAR CreateSetReparsePointRequest(PIRP Irp,
                                   PUNICODE_STRING SymbolicLinkName,
                                   PULONG Length) {
  UNREFERENCED_PARAMETER(Irp);
  USHORT mountPointReparsePathLength =
      SymbolicLinkName->Length + sizeof(WCHAR) /* "\\" */;
  *Length =
      FIELD_OFFSET(REPARSE_DATA_BUFFER, MountPointReparseBuffer.PathBuffer) +
      mountPointReparsePathLength + sizeof(WCHAR) + sizeof(WCHAR);
  PREPARSE_DATA_BUFFER reparseData = DokanAllocZero(*Length);
  if (!reparseData) {
    DOKAN_LOG_FINE_IRP(Irp, "Failed to allocate reparseData buffer");
    *Length = 0;
    return NULL;
  }

  reparseData->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
  reparseData->ReparseDataLength =
      (USHORT)(*Length) - REPARSE_DATA_BUFFER_HEADER_SIZE;
  reparseData->MountPointReparseBuffer.SubstituteNameOffset = 0;
  reparseData->MountPointReparseBuffer.SubstituteNameLength =
      mountPointReparsePathLength;
  reparseData->MountPointReparseBuffer.PrintNameOffset =
      reparseData->MountPointReparseBuffer.SubstituteNameLength + sizeof(WCHAR);
  reparseData->MountPointReparseBuffer.PrintNameLength = 0;
  // SET_REPARSE expect a path ending with a backslash
  // We add it manually to our PersistanteSymbolicLink: \??\Volume{GUID}
  RtlCopyMemory(reparseData->MountPointReparseBuffer.PathBuffer,
                SymbolicLinkName->Buffer, SymbolicLinkName->Length);
  reparseData->MountPointReparseBuffer
      .PathBuffer[mountPointReparsePathLength / sizeof(WCHAR) - 1] = L'\\';

  return (PCHAR)reparseData;
}

PCHAR CreateRemoveReparsePointRequest(PIRP Irp, PULONG Length) {
  UNREFERENCED_PARAMETER(Irp);
  *Length = REPARSE_GUID_DATA_BUFFER_HEADER_SIZE;
  PREPARSE_DATA_BUFFER reparseData =
      DokanAllocZero(sizeof(REPARSE_DATA_BUFFER));
  if (!reparseData) {
    DOKAN_LOG_FINE_IRP(Irp, "Failed to allocate reparseGuidData buffer");
    *Length = 0;
    return NULL;
  }
  reparseData->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
  return (PCHAR)reparseData;
}

NTSTATUS SendDirectoryFsctl(PIRP Irp, PDEVICE_OBJECT DeviceObject, PUNICODE_STRING Path,
                            ULONG Code, PCHAR Input, ULONG Length) {
  UNREFERENCED_PARAMETER(Irp);
  HANDLE handle = 0;
  PUNICODE_STRING directoryStr = NULL;
  DOKAN_INIT_LOGGER(logger, DeviceObject->DriverObject,
                    IRP_MJ_FILE_SYSTEM_CONTROL);

  __try {
    // Convert Dcb MountPoint \DosDevices\C:\foo to \??\C:\foo
    directoryStr = ChangePrefix(Path, &g_DosDevicesPrefix, TRUE /*HasPrefix*/,
                                &g_ObjectManagerPrefix);
    if (!directoryStr) {
      return DokanLogError(&logger, STATUS_INVALID_PARAMETER,
                           L"Failed to change prefix for \"%wZ\"\n", Path);
    }

    // Open the directory as \??\C:\foo
    IO_STATUS_BLOCK ioStatusBlock;
    OBJECT_ATTRIBUTES objectAttributes;
    InitializeObjectAttributes(&objectAttributes, directoryStr,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL,
                               NULL);
    DOKAN_LOG_FINE_IRP(Irp, "Open directory \"%wZ\"", directoryStr);
    NTSTATUS result = ZwOpenFile(
        &handle, FILE_WRITE_ATTRIBUTES, &objectAttributes, &ioStatusBlock,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_OPEN_REPARSE_POINT | FILE_OPEN_FOR_BACKUP_INTENT);
    if (!NT_SUCCESS(result)) {
      return DokanLogError(
          &logger, result,
          L"SendDirectoryFsctl - ZwOpenFile failed to open\"%wZ\"\n",
          directoryStr);
    }

    result = ZwFsControlFile(handle, NULL, NULL, NULL, &ioStatusBlock, Code,
                             Input, Length, NULL, 0);
    if (!NT_SUCCESS(result)) {
      return DokanLogError(
          &logger, result,
          L"SendDirectoryFsctl - ZwFsControlFile Code %X on \"%wZ\" failed\n", Code,
          directoryStr);
    }
  } __finally {
   if (directoryStr) {
      DokanFreeUnicodeString(directoryStr);
    }
    if (handle) {
      ZwClose(handle);
    }
  }

  DOKAN_LOG_FINE_IRP(Irp, "Success");
  return STATUS_SUCCESS;
}

// TODO(adrienj): Change DDbgPrint in this function to DokanLogInfo when we will
// better logging.
NTSTATUS DokanMountVolume(__in PDEVICE_OBJECT DiskDevice, __in PIRP Irp) {
  PDokanDCB dcb = NULL;
  PDokanVCB vcb = NULL;
  PVPB vpb = NULL;
  DOKAN_CONTROL dokanControl;
  PMOUNT_ENTRY mountEntry = NULL;
  PIO_STACK_LOCATION irpSp;
  PDEVICE_OBJECT volDeviceObject;
  PDRIVER_OBJECT driverObject = DiskDevice->DriverObject;
  NTSTATUS status = STATUS_UNRECOGNIZED_VOLUME;

  DOKAN_INIT_LOGGER(logger, driverObject, IRP_MJ_FILE_SYSTEM_CONTROL);
  DOKAN_LOG_FINE_IRP(Irp, "Mounting disk device.");

  irpSp = IoGetCurrentIrpStackLocation(Irp);
  dcb = irpSp->Parameters.MountVolume.DeviceObject->DeviceExtension;
  if (!dcb) {
    DOKAN_LOG_FINE_IRP(Irp, "Not DokanDiskDevice (no device extension)");
    return status;
  }

  if (GetIdentifierType(dcb) != DCB) {
    DOKAN_LOG_FINE_IRP(Irp, "Not DokanDiskDevice");
    return status;
  }

  if (IsDeletePending(dcb->DeviceObject)) {
    return DokanLogError(&logger, STATUS_DEVICE_REMOVED,
                         L"This is a remount try of the device.");
  }

  BOOLEAN isNetworkFileSystem =
      (dcb->VolumeDeviceType == FILE_DEVICE_NETWORK_FILE_SYSTEM);

  DokanLogInfo(&logger,
               L"Mounting volume using MountPoint \"%wZ\" device \"%wZ\"",
               dcb->MountPoint, dcb->DiskDeviceName);

  if (!isNetworkFileSystem) {
    status = IoCreateDevice(driverObject,               // DriverObject
                            sizeof(DokanVCB),           // DeviceExtensionSize
                            NULL,                       // DeviceName
                            dcb->VolumeDeviceType,      // DeviceType
                            dcb->DeviceCharacteristics, // DeviceCharacteristics
                            FALSE,                      // Not Exclusive
                            &volDeviceObject);          // DeviceObject
  } else {
    status = IoCreateDeviceSecure(
        driverObject,               // DriverObject
        sizeof(DokanVCB),           // DeviceExtensionSize
        dcb->DiskDeviceName,        // DeviceName
        dcb->VolumeDeviceType,      // DeviceType
        dcb->DeviceCharacteristics, // DeviceCharacteristics
        FALSE,                      // Not Exclusive
        &sddl,                      // Default SDDL String
        NULL,                       // Device Class GUID
        &volDeviceObject);          // DeviceObject
  }

  if (!NT_SUCCESS(status)) {
    return DokanLogError(&logger, status, L"IoCreateDevice failed.");
  }

  vcb = volDeviceObject->DeviceExtension;
  vcb->Identifier.Type = VCB;
  vcb->Identifier.Size = sizeof(DokanVCB);

  vcb->DeviceObject = volDeviceObject;
  vcb->Dcb = dcb;
  vcb->ResourceLogger.DriverObject = driverObject;
  vcb->ValidFcbMask = 0xffffffffffffffff;
  dcb->Vcb = vcb;

  if (vcb->Dcb->FcbGarbageCollectionIntervalMs != 0) {
    InitializeListHead(&vcb->FcbGarbageList);
    KeInitializeEvent(&vcb->FcbGarbageListNotEmpty, SynchronizationEvent,
                      FALSE);
    DokanStartFcbGarbageCollector(vcb);
  }

  InitializeListHead(&vcb->NextFCB);

  InitializeListHead(&vcb->DirNotifyList);
  FsRtlNotifyInitializeSync(&vcb->NotifySync);

  ExInitializeFastMutex(&vcb->AdvancedFCBHeaderMutex);

  FsRtlSetupAdvancedHeader(&vcb->VolumeFileHeader,
                           &vcb->AdvancedFCBHeaderMutex);

  vpb = irpSp->Parameters.MountVolume.Vpb;
  DokanInitVpb(vpb, vcb->DeviceObject);

  //
  // Establish user-buffer access method.
  //
  SetLongFlag(volDeviceObject->Flags, DO_DIRECT_IO);
  ClearLongFlag(volDeviceObject->Flags, DO_DEVICE_INITIALIZING);
  SetLongFlag(vcb->Flags, VCB_MOUNTED);

  ObReferenceObject(volDeviceObject);

  DOKAN_LOG_FINE_IRP(Irp, "ExAcquireResourceExclusiveLite dcb resource");
  ExAcquireResourceExclusiveLite(&dcb->Resource, TRUE);

  // set the device on dokanControl
  RtlZeroMemory(&dokanControl, sizeof(DOKAN_CONTROL));
  RtlCopyMemory(dokanControl.DeviceName, dcb->DiskDeviceName->Buffer,
                dcb->DiskDeviceName->Length);
  if (dcb->UNCName->Buffer != NULL && dcb->UNCName->Length > 0) {
    RtlCopyMemory(dokanControl.UNCName, dcb->UNCName->Buffer,
                  dcb->UNCName->Length);
  }
  dokanControl.SessionId = dcb->SessionId;
  mountEntry = FindMountEntry(dcb->Global, &dokanControl, TRUE);
  if (mountEntry != NULL) {
    mountEntry->MountControl.VolumeDeviceObject = volDeviceObject;
    mountEntry->MountControl.MountOptions = dcb->MountOptions;
  } else {
    ExReleaseResourceLite(&dcb->Resource);
    return DokanLogError(&logger, STATUS_DEVICE_REMOVED,
                         L"MountEntry not found.");
  }

  ExReleaseResourceLite(&dcb->Resource);

  // Start check thread
  ExAcquireResourceExclusiveLite(&dcb->Resource, TRUE);
  DokanUpdateTimeout(&dcb->TickCount, DOKAN_KEEPALIVE_TIMEOUT_DEFAULT * 3);
  ExReleaseResourceLite(&dcb->Resource);
  DokanStartCheckThread(dcb);

  BOOLEAN isDriveLetter = IsMountPointDriveLetter(dcb->MountPoint);
  // Create mount point for the volume
  if (dcb->UseMountManager) {
    BOOLEAN autoMountStateBackup = TRUE;
    if (!isDriveLetter) {
      ExAcquireResourceExclusiveLite(&dcb->Global->MountManagerLock, TRUE);
      // Query current AutoMount State to restore it afterward.
      DokanQueryAutoMount(&autoMountStateBackup);
      // In case of failure, we suppose it was Enabled.

      // MountManager suggest workflow do not accept a path longer than
      // a driver letter mount point so we cannot use it to suggest
      // our directory mount point. We disable Mount Manager AutoMount
      // for avoiding having a driver letter assign to our device
      // for the time we create our own mount point.
      if (autoMountStateBackup) {
        DokanSendAutoMount(FALSE);
      }
    }
    status = DokanSendVolumeArrivalNotification(dcb->DiskDeviceName);
    if (!NT_SUCCESS(status)) {
      DokanLogError(&logger, status,
                    L"DokanSendVolumeArrivalNotification failed.");
    }
    if (!isDriveLetter) {
      // Restore previous AutoMount state.
      if (autoMountStateBackup) {
        DokanSendAutoMount(TRUE);
      }
      ExReleaseResourceLite(&dcb->Global->MountManagerLock);
    }
  }

  if (isDriveLetter) {
    DokanCreateMountPoint(dcb);
  }

  if (isNetworkFileSystem) {
    RunAsSystem(DokanRegisterUncProvider, dcb);
  }

  DokanLogInfo(&logger, L"Mounting successfully done.");
  DOKAN_LOG_FINE_IRP(Irp, "Mounting successfully done.");

  return STATUS_SUCCESS;
}

VOID DokanInitVpb(__in PVPB Vpb, __in PDEVICE_OBJECT VolumeDevice) {
  if (Vpb != NULL) {
    Vpb->DeviceObject = VolumeDevice;
    Vpb->VolumeLabelLength = (USHORT)wcslen(VOLUME_LABEL) * sizeof(WCHAR);
    RtlStringCchCopyW(Vpb->VolumeLabel,
                      sizeof(Vpb->VolumeLabel) / sizeof(WCHAR), VOLUME_LABEL);
    Vpb->SerialNumber = 0x19831116;
  }
}

NTSTATUS
DokanDispatchFileSystemControl(__in PDEVICE_OBJECT DeviceObject,
                               __in PIRP Irp) {
  NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
  PIO_STACK_LOCATION irpSp;

  __try {
    DOKAN_LOG_BEGIN_MJ(Irp);
    irpSp = IoGetCurrentIrpStackLocation(Irp);
    switch (irpSp->MinorFunction) {
    case IRP_MN_MOUNT_VOLUME: {
      status = DokanMountVolume(DeviceObject, Irp);
    } break;

    case IRP_MN_USER_FS_REQUEST:
      status = DokanUserFsRequest(DeviceObject, &Irp);
      break;
    default:
      DOKAN_LOG_FINE_IRP(Irp, "Unsupported MinorFunction %x",
                         irpSp->MinorFunction);
    }
  } __finally {
    DOKAN_LOG_END_MJ(Irp, status, 0);
    DokanCompleteIrpRequest(Irp, status, 0);
  }

  return status;
}
