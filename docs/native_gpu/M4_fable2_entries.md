| function | engine sites | insns | args read | dev offsets read (r3) | dev offsets written | PM4 | library callees | imports |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| sub_82BA1D40 | 21 | 103 | r3 r4 r5 | 48 56 10940 12187 12440 12444 12448 12452 12456 12708 | 4 48 10932 10936 10940 12708 12712 12716 | SET_BIN_MASK_HI, SET_BIN_MASK_LO |  |  |
| sub_82BA2218 | 10 | 21 | r3 | -4 60 | 60 | - | sub_82BA6C18 |  |
| sub_82BA1AB0 | 9 | 49 | r3 | 10941 13516 13520 13524 16728 | 13520 13524 13528 | - |  |  |
| sub_82B9DA70 | 9 | 37 | r3 | 0 |  | - | sub_82B9C540 |  |
| sub_82BA27C8 | 8 | 25 | r3 | 8 |  | - |  | KeGetCurrentProcessType |
| sub_82B9DBA0 | 7 | 12 | r3 r4 | 0 |  | - |  |  |
| sub_82B9FA98 | 7 | 147 | r3 | 0 4 12 16 28 88 152 | 8 12 | - |  |  |
| sub_82B99720 | 6 | 33 | r3 | 0 |  | - | sub_82B99630 |  |
| sub_82B9D400 | 5 | 45 | r3 | 19888 19896 19908 19920 19940 | 19888 19896 | - | sub_82B9D360 |  |
| sub_82B9EB48 | 5 | 2 |  |  |  | - | sub_82B9E8C8 |  |
| sub_82B9D970 | 4 | 5 | r3 | 0 |  | - | sub_82B9B350 |  |
| sub_82B9DBD0 | 4 | 5 | r3 | 0 |  | - | sub_82B9B578 |  |
| sub_82B991B0 | 4 | 21 | r3 r4 r5 r6 | 32 | 32 | - |  |  |
| sub_82B9DD48 | 4 | 53 | r3 r4 r5 r6 | 19900 | 19900 | - | sub_82B99948 sub_82B9E008 |  |
| sub_82B99208 | 4 | 21 | r3 r4 r5 r6 | 32 | 32 | - |  |  |
| sub_82B99260 | 4 | 20 | r3 r4 | 0 4 8 |  | - |  |  |
| sub_82B9DC60 | 4 | 22 | r3 | 0 19892 | 19904 | - | sub_82B9A168 sub_82B9D188 |  |
| sub_82B9D4B8 | 4 | 1 |  |  |  | - | sub_82B9DED8 |  |
| sub_82B9D6B8 | 4 | 37 | r3 r4 r5 | 0 |  | - |  |  |
| sub_82B9D8A0 | 3 | 22 | r3 r4 | 0 |  | - | sub_82B9C858 sub_82B9EB50 |  |
| sub_82BA1EE0 | 3 | 49 | r3 | 0 4 16 20 |  | - |  |  |
| sub_82B9D840 | 3 | 23 | r3 r5 | 0 |  | - | sub_82B9ECF8 |  |
| sub_82BA2830 | 3 | 284 | r3 r4 | 48 56 10896 10900 10908 10984 10988 14836 14840 14904 | 4 48 52 56 10896 10900 10908 10924 10926 10952 | - | sub_82BA25C0 sub_82BA2748 | KiApcNormalRoutineNop MmGetPhysicalAddress VdEnableRingBufferRPtrWriteBack VdInitializeRingBuffer VdSetSystemCommandBufferGpuIdentifierAddress |
| sub_82B9E1F0 | 3 | 19 |  |  |  | - | sub_82B9DE20 |  |
| sub_82B9D958 | 3 | 5 | r3 | 0 |  | - | sub_82B9B1C0 |  |
| sub_82B9D4E0 | 3 | 5 | r3 | 0 |  | - | sub_82B9A168 |  |
| sub_82BA6C18 | 3 | 168 | r3 | 48 10940 11804 13780 16728 21572 21636 21640 23984 23992 | 10948 10984 10988 21636 21640 21644 21648 23984 23992 | - | sub_82BA23E8 sub_82BA2748 sub_82BA2830 sub_82BA67C8 sub_82BA | ExRegisterTitleTerminateNotification KeGetCurrentProcessType KeSetEvent VdSetGraphicsInterruptCallback VdSetSystemCommandBufferGpuIdentifierAddress VdShutdownEngines |
| sub_82BA2F68 | 3 | 119 | r3 r4 | 48 56 10900 10941 11852 13596 | 4 48 10941 | WAIT_REG_MEM | sub_82BAA848 |  |
| sub_82B9D750 | 3 | 21 | r3 r4 r5 | 19900 |  | - | sub_82B9EDC0 |  |
| sub_82B9D4F8 | 3 | 7 | r3 r4 r5 | 0 |  | - |  |  |
| sub_82B9D8F8 | 3 | 23 | r3 r4 r5 | 0 |  | - | sub_82B9C858 sub_82B9EB50 |  |
| sub_82B9DB70 | 3 | 5 | r3 | 0 |  | - |  |  |
| sub_82B9D990 | 3 | 50 | r3 r4 r5 | 19900 |  | - | sub_82B9EDC0 |  |
| sub_82BA34D8 | 3 | 457 | r3 r4 | 48 56 10896 10908 10941 10942 10943 13544 13548 13608 | 4 48 10928 10941 10942 10943 14940 16560 16708 21544 | REG_RMW | sub_82BA1EE0 sub_82BA1FA8 sub_82BA2F68 sub_82BA3148 sub_82BA | KeGetCurrentProcessType MmFreePhysicalMemory VdGetCurrentDisplayInformation VdGetSystemCommandBuffer VdPersistDisplay VdSetDisplayMode VdSwap |
| sub_82B9E750 | 3 | 93 | r3 r4 r5 r6 r9 r10 | 0 4 8 | 0 | - | sub_82B9E240 sub_82B9E3D8 |  |
| sub_82B9DA58 | 3 | 5 | r3 | 0 |  | - | sub_82B9B488 |  |
| sub_82B9F970 | 3 | 73 | r3 r4 r5 | 21560 |  | - |  |  |
| sub_82B9D7A8 | 3 | 38 | r3 r4 r6 | 0 |  | - | sub_82B9ECF8 |  |
| sub_82BA1FA8 | 3 | 54 | r3 | 0 8 12 | 8 12 | - | sub_82BA75F0 |  |
| sub_82B9D988 | 3 | 1 |  |  |  | - | sub_82B9D068 |  |
| sub_82B9D638 | 3 | 31 | r3 r4 r5 | 0 |  | - |  |  |
| sub_82BA49B0 | 2 | 81 | r3 r4 r5 |  |  | - |  |  |
| sub_82B9D4C0 | 2 | 8 | r3 r4 | 19892 | 0 19892 | - |  |  |
| sub_82B9EE58 | 2 | 25 | r4 |  |  | - |  | KeGetCurrentProcessType |
| sub_82B9D540 | 2 | 30 | r3 r4 | 19896 | 19896 | - |  |  |
| sub_82BA40F0 | 2 | 98 | r3 r5 r6 r7 | 32 872 896 | 872 | - | sub_82BA3D88 sub_82BA3EC8 sub_82BA3FF0 |  |
| sub_82B9DCB8 | 2 | 36 | r3 | 19900 |  | - | sub_82B9E008 |  |
| sub_82B9D5B8 | 2 | 31 | r3 r4 | 0 |  | - |  |  |
| sub_82BA1A08 | 2 | 41 |  |  |  | - |  |  |
| sub_82B9EEE0 | 2 | 85 | r3 | 16 48 56 | 4 16 48 | DRAW_INDX_2, INVALIDATE_STATE |  |  |
| sub_82BA2548 | 2 | 29 | r3 | 10941 16728 | 48 52 56 10941 | - |  | KeGetCurrentProcessType |
| sub_82B9F940 | 2 | 5 | r4 |  |  | - |  |  |
| sub_82B9D518 | 2 | 10 | r3 r4 | 0 |  | - |  |  |
| sub_82BA22C0 | 2 | 2 | r3 | 10888 |  | - |  |  |
| sub_82BA2200 | 1 | 5 | r3 | 60 | 60 | - |  |  |
| sub_82BA6298 | 1 | 156 | r3 | 10940 12187 12440 12444 12448 12452 12456 12728 12732 12736 | 10932 10936 10940 12192 12196 12708 12712 12716 13596 23980 | - | sub_82B995A8 |  |
| sub_82BA6508 | 1 | 176 | r3 r4 |  | 13608 14824 14828 14832 16708 21544 21548 21552 | - | sub_82BAEC38 | VdQueryVideoMode |
| sub_82B9DB08 | 1 | 26 | r3 | 0 |  | - |  |  |
| sub_82BA2360 | 1 | 33 | r3 | 10942 | 10888 10942 | - |  | RtlEnterCriticalSection RtlLeaveCriticalSection |
| sub_82BA1B78 | 1 | 49 | r3 | 10941 13516 13532 13536 16728 | 13532 13536 13540 | - |  |  |
| sub_82BA67C8 | 1 | 43 | r3 | 48 14824 14828 14832 | 14824 14828 14832 | - | sub_82BA2748 sub_82BAC2C8 | KeGetCurrentProcessType |
| sub_82BA2748 | 1 | 31 | r3 | 48 56 10908 11008 | 4 48 | - |  |  |
| sub_82B9DBE8 | 1 | 29 | r3 | 0 |  | - |  |  |
| sub_82B9E8C8 | 1 | 160 | r3 r4 r5 | 0 4 |  | - |  |  |
| sub_82B9DB88 | 1 | 6 | r3 | 0 |  | - |  |  |
| sub_82B9EED0 | 1 | 3 | r3 | 24 |  | - |  |  |
| sub_82B9F958 | 1 | 5 | r4 |  |  | - |  |  |
| sub_82BA60C8 | 1 | 116 | r3 | 48 56 10564 10568 23980 | 0 4 8 16 24 32 48 10428 10444 10564 | REG_RMW |  |  |
| sub_82BA22B0 | 1 | 4 | r3 |  | 10888 | - |  |  |
| sub_82B994B8 | 1 | 60 | r3 | 0 4 8 |  | - | sub_82B993C0 |  |
| sub_82BA45E0 | 1 | 38 | r3 r4 r5 |  |  | - | sub_82BA4328 |  |
| sub_82BA4678 | 1 | 80 | r3 r4 r5 |  |  | - | sub_82BA4328 |  |
| sub_82BA2270 | 1 | 16 | r3 | 10888 | 10888 | - |  |  |
| sub_82BA4AF8 | 1 | 132 | r3 r4 |  |  | - | sub_82BA49B0 | RtlEnterCriticalSection RtlInitializeCriticalSection RtlLeaveCriticalSection |
| sub_82B9F3C0 | 1 | 19 | r3 |  |  | - |  |  |
| sub_82B9F550 | 1 | 251 | r5 r6 r8 |  |  | - | sub_82B9F410 |  |
| sub_82B992B0 | 1 | 68 | r3 | 0 4 8 |  | - |  |  |
| sub_82B993C0 | 1 | 62 | r3 r4 | 32 872 | 0 4 20 32 | - |  |  |
| sub_82B9EEC0 | 1 | 4 | r3 | 24 |  | - |  |  |
| sub_82B997A8 | 1 | 1 |  |  |  | - |  |  |
| sub_82BA3D88 | 1 | 80 | r3 r4 r5 r6 | 20 24 28 |  | - | sub_82BA3C00 sub_82BA3CD0 |  |
| sub_82B997B0 | 1 | 4 | r3 r4 |  |  | - |  |  |
| sub_82BA1C40 | 1 | 63 | r3 | 108 152 156 164 176 |  | - |  | KeGetCurrentProcessType RtlEnterCriticalSection RtlLeaveCriticalSection |
| sub_82BA22C8 | 1 | 38 | r3 | 10942 11320 | 10888 10942 | - | sub_82BA2748 | RtlEnterCriticalSection RtlLeaveCriticalSection |
| sub_82BA2500 | 1 | 17 | r3 | 10940 13516 |  | - |  |  |
| sub_82BA6990 | 1 | 162 | r3 r4 | 10942 | 10942 16712 21560 21564 21588 21592 | - | sub_82B99018 sub_82BA2080 sub_82BA2830 sub_82BA5F48 sub_82BA | ExGetXConfigSetting ExRegisterTitleTerminateNotification KeGetCurrentProcessType RtlInitializeCriticalSection VdInitializeEngines VdIsHSIOTrainingSucceeded VdSetGraphicsInterruptCallback |
| sub_82B98F00 | 1 | 70 | r3 | 16 24 32 10436 10440 | 16 24 32 | - |  |  |
| sub_82B9F038 | 1 | 226 | r3 | 48 56 | 4 48 | DRAW_INDX_2, IM_LOAD_IMMEDIATE, INVALIDATE_STATE |  |  |
| sub_82BAE440 | 0 | 328 | r3 r4 r5 r6 r7 r8 r9 r10 | 21548 21552 |  | - | sub_82BAE198 | VdQueryVideoMode |
| sub_82BAEA88 | 0 | 108 | r3 r4 r5 r7 | 21544 21548 21552 | 48 | - | sub_82BAE440 sub_82BAE960 | RtlFillMemoryUlong VdInitializeScalerCommandBuffer |
| sub_82BAC2C8 | 0 | 275 | r3 | 23968 |  | - | sub_82BA45E0 sub_82BA4678 sub_82BAB100 sub_82BABEA0 | KeGetCurrentProcessType KeQueryPerformanceFrequency ObDeleteSymbolicLink VdGetCurrentDisplayInformation |
| sub_82BAED78 | 0 | 57 | r3 r4 r5 |  | 48 | - | sub_82BA2748 | KeEnterCriticalRegion KeLeaveCriticalRegion RtlEnterCriticalSection RtlLeaveCriticalSection VdRetrainEDRAM VdRetrainEDRAMWorker |
| sub_82BABF58 | 0 | 220 | r3 r4 | 23968 |  | - | sub_82BAB210 sub_82BAC2C8 | ObCreateSymbolicLink RtlInitAnsiString VdGetCurrentDisplayInformation sprintf |
| sub_82BAAA28 | 0 | 184 | r3 | 10942 16720 21580 21584 21600 21608 21636 21640 21644 21648 | 10942 16720 16724 21576 21580 21584 21600 21608 21616 21620 | - | sub_82B9FCE8 sub_82BA0380 sub_82BA2748 sub_82BAA2B8 sub_82BA | VdEnableDisableClockGating |
| sub_82BAE960 | 0 | 73 | r3 r4 r5 r6 r7 | 13588 21544 21548 |  | - |  | VdQueryVideoFlags |
| sub_82BA95E0 | 0 | 75 | r3 r4 r5 r6 r7 r8 r9 r10 | 48 56 10941 21536 | 48 21680 21684 21688 21692 21696 21700 | - | sub_82BA8EF8 | VdQueryVideoMode |
| sub_82BA4328 | 0 | 173 | r3 |  |  | - | sub_82BA4278 | VdGetCurrentDisplayGamma |
| sub_82BAEC38 | 0 | 79 | r3 r4 | 10942 13544 13548 14828 21544 21548 | 10942 | - | sub_82BAE960 | VdCallGraphicsNotificationRoutines |
| sub_82BA6968 | 0 | 10 |  |  |  | - |  | ExRegisterTitleTerminateNotification VdShutdownEngines |
