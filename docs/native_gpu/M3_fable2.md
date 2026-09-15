# Native GPU M3 recon - fable2recomp

functions: 60330   shared-fingerprint with ng2recomp: 682

Direct3D library address range(s) (clusters holding the Vd* callers): 82B98D28-82BAED78

engine-side PM4 emitter clusters (inline XDK code in game functions): 821F0E00-82207AF8, 82217DB8-8222BFA0, 82298630-822B4008, 831BA458-831BA810

engine functions that emit PM4 themselves (outside the library): 33

PM4 type-3 emitters: 51   Vd* callers: 16   library functions in range: 234 (shared 5)   API surface (library functions called by engine code): 88


## PM4 emitters (function, shared?, opcodes, #engine callers)

- sub_822655F0  game-only  len 91  [SET_BIN_MASK_LO]  callers 25 (engine 0)
- sub_82206888  game-only  len 912  [IM_LOAD_IMMEDIATE, INVALIDATE_STATE, SET_BIN_MASK_HI, SET_BIN_MASK_LO, WAIT_REG_MEM]  callers 23 (engine 0)
- sub_82BA1D40  game-only  len 103  [SET_BIN_MASK_HI, SET_BIN_MASK_LO]  callers 21 (engine 21)
- sub_8221C3E8  game-only  len 252  [SET_BIN_MASK_LO]  callers 11 (engine 0)
- sub_8221DFC0  game-only  len 276  [SET_BIN_MASK_LO]  callers 8 (engine 0)
- sub_821F6050  game-only  len 135  [WAIT_REG_MEM]  callers 6 (engine 0)
- sub_8221B010  game-only  len 548  [SET_BIN_MASK_HI]  callers 6 (engine 0)
- sub_822B4008  game-only  len 95  [WAIT_REG_MEM]  callers 6 (engine 0)
- sub_82BA2F68  game-only  len 119  [WAIT_REG_MEM]  callers 6 (engine 3)
- sub_822A6558  game-only  len 61  [SET_BIN_MASK_HI, SET_BIN_MASK_LO, SET_BIN_SELECT_HI, SET_BIN_SELECT_LO]  callers 6 (engine 0)
- sub_821F9918  game-only  len 188  [SET_BIN_MASK_HI]  callers 6 (engine 0)
- sub_82205F68  game-only  len 294  [SET_BIN_MASK_LO]  callers 5 (engine 0)
- sub_822194B8  game-only  len 162  [EVENT_WRITE, EVENT_WRITE_EXT, MEM_WRITE, WAIT_REG_MEM]  callers 4 (engine 0)
- sub_82217DB8  game-only  len 330  [SET_BIN_MASK_LO]  callers 4 (engine 0)
- sub_82BA34D8  game-only  len 457  [REG_RMW]  callers 4 (engine 3)
- sub_82BA3148  game-only  len 41  [REG_RMW]  callers 4 (engine 0)
- sub_82196750  game-only  len 140  [EVENT_WRITE]  callers 4 (engine 0)
- sub_822866E0  game-only  len 121  [INDIRECT_BUFFER]  callers 3 (engine 0)
- sub_82283830  game-only  len 125  [WAIT_REG_MEM]  callers 2 (engine 0)
- sub_82298630  game-only  len 59  [EVENT_WRITE_SHD]  callers 2 (engine 0)
- sub_82BA60C8  game-only  len 116  [REG_RMW]  callers 2 (engine 1)
- sub_82B9EEE0  game-only  len 85  [DRAW_INDX_2, INVALIDATE_STATE]  callers 2 (engine 2)
- sub_82BA7B28  game-only  len 550  [DRAW_INDX_2, IM_LOAD_IMMEDIATE, INVALIDATE_STATE]  callers 1 (engine 0)
- sub_82BA2080  game-only  len 96  [IM_LOAD_IMMEDIATE, INVALIDATE_STATE]  callers 1 (engine 0)
- sub_82BA4FB8  game-only  len 56  [SET_BIN_SELECT_LO]  callers 1 (engine 0)
- sub_8221AE18  game-only  len 126  [EVENT_WRITE]  callers 1 (engine 0)
- sub_821F0E00  game-only  len 346  [SET_BIN_MASK_LO]  callers 1 (engine 0)
- sub_8222BFA0  game-only  len 81  [LOAD_ALU_CONSTANT]  callers 1 (engine 0)
- sub_82BAA848  game-only  len 54  [EVENT_WRITE_SHD]  callers 1 (engine 0)
- sub_82BA0380  game-only  len 1441  [REG_TO_MEM]  callers 1 (engine 0)
- sub_82D45AE0  game-only  len 72  [LOAD_ALU_CONSTANT]  callers 1 (engine 0)
- sub_82FE8ED0  game-only  len 75  [LOAD_ALU_CONSTANT]  callers 1 (engine 0)
- sub_82B98D28  game-only  len 118  [IM_LOAD_IMMEDIATE, INVALIDATE_STATE]  callers 1 (engine 0)
- sub_822192E8  game-only  len 115  [EVENT_WRITE_ZPD, SET_BIN_MASK_HI, SET_BIN_MASK_LO]  callers 1 (engine 0)
- sub_82D46930  game-only  len 72  [LOAD_ALU_CONSTANT]  callers 1 (engine 0)
- sub_82BA83C0  game-only  len 345  [DRAW_INDX_2, IM_LOAD_IMMEDIATE, INVALIDATE_STATE]  callers 1 (engine 0)
- sub_830470C0  game-only  len 970  [SET_BIN_MASK_LO]  callers 1 (engine 0)
- sub_82B9F038  game-only  len 226  [DRAW_INDX_2, IM_LOAD_IMMEDIATE, INVALIDATE_STATE]  callers 1 (engine 1)
- sub_821E0220  game-only  len 90  [IM_LOAD_IMMEDIATE, INVALIDATE_STATE]  callers 1 (engine 0)
- sub_82BA4D80  game-only  len 142  [EVENT_WRITE_SHD]  callers 1 (engine 0)
- sub_822A61C0  game-only  len 229  [EVENT_WRITE]  callers 1 (engine 0)
- sub_82B99018  game-only  len 102  [DRAW_INDX_2]  callers 1 (engine 0)
- sub_8222B8F0  game-only  len 427  [IM_LOAD_IMMEDIATE, INVALIDATE_STATE, SET_BIN_MASK_LO]  callers 1 (engine 0)
- sub_8221C898  game-only  len 259  [SET_BIN_MASK_LO]  callers 1 (engine 0)
- sub_82FE94F8  game-only  len 75  [LOAD_ALU_CONSTANT]  callers 1 (engine 0)
- sub_82207AF8  game-only  len 330  [SET_BIN_MASK_LO]  callers 1 (engine 0)
- sub_82BA48B0  game-only  len 64  [COND_WRITE]  callers 1 (engine 0)
- sub_82BAC718  game-only  len 580  [EVENT_WRITE, IM_LOAD_IMMEDIATE, INVALIDATE_STATE]  callers 1 (engine 0)
- sub_831BA458  game-only  len 118  [SET_STATE]  callers 0 (engine 0)
- sub_831BA630  game-only  len 119  [IM_LOAD, SET_STATE]  callers 0 (engine 0)
- sub_831BA810  game-only  len 247  [IM_LOAD, IM_LOAD_IMMEDIATE, SET_STATE]  callers 0 (engine 0)

## Vd* kernel callers

- sub_82BA2830  game-only  VdEnableRingBufferRPtrWriteBack, VdInitializeRingBuffer, VdSetSystemCommandBufferGpuIdentifierAddress  callers 5
- sub_82BA34D8  game-only  VdGetCurrentDisplayInformation, VdGetSystemCommandBuffer, VdPersistDisplay, VdSetDisplayMode, VdSwap  callers 4
- sub_82BA4328  game-only  VdGetCurrentDisplayGamma  callers 2
- sub_82BA6508  game-only  VdQueryVideoMode  callers 2
- sub_82BA6968  game-only  VdShutdownEngines  callers 0
- sub_82BA6990  game-only  VdInitializeEngines, VdIsHSIOTrainingSucceeded, VdSetGraphicsInterruptCallback  callers 1
- sub_82BA6C18  game-only  VdSetGraphicsInterruptCallback, VdSetSystemCommandBufferGpuIdentifierAddress, VdShutdownEngines  callers 4
- sub_82BA95E0  game-only  VdQueryVideoMode  callers 1
- sub_82BAAA28  game-only  VdEnableDisableClockGating  callers 1
- sub_82BABF58  game-only  VdGetCurrentDisplayInformation  callers 1
- sub_82BAC2C8  game-only  VdGetCurrentDisplayInformation  callers 4
- sub_82BAE440  game-only  VdQueryVideoMode  callers 1
- sub_82BAE960  game-only  VdQueryVideoFlags  callers 2
- sub_82BAEA88  game-only  VdInitializeScalerCommandBuffer  callers 3
- sub_82BAEC38  game-only  VdCallGraphicsNotificationRoutines  callers 1
- sub_82BAED78  game-only  VdRetrainEDRAM, VdRetrainEDRAMWorker  callers 2

## API surface: library entry points called from engine code (sorted by call sites)

- sub_82BA1D40  game-only  len 103  pm4 [SET_BIN_MASK_HI, SET_BIN_MASK_LO]  engine call sites 21
- sub_82BA2218  shared  len 21  pm4 [-]  engine call sites 10
- sub_82BA1AB0  game-only  len 49  pm4 [-]  engine call sites 9
- sub_82B9DA70  game-only  len 37  pm4 [-]  engine call sites 9
- sub_82BA27C8  game-only  len 25  pm4 [-]  engine call sites 8
- sub_82B9FA98  game-only  len 147  pm4 [-]  engine call sites 7
- sub_82B9DBA0  game-only  len 12  pm4 [-]  engine call sites 7
- sub_82B99720  game-only  len 33  pm4 [-]  engine call sites 6
- sub_82B9D400  game-only  len 45  pm4 [-]  engine call sites 5
- sub_82B9EB48  game-only  len 2  pm4 [-]  engine call sites 5
- sub_82B9D6B8  game-only  len 37  pm4 [-]  engine call sites 4
- sub_82B9DBD0  game-only  len 5  pm4 [-]  engine call sites 4
- sub_82B9D4B8  game-only  len 1  pm4 [-]  engine call sites 4
- sub_82B99260  game-only  len 20  pm4 [-]  engine call sites 4
- sub_82B9DC60  game-only  len 22  pm4 [-]  engine call sites 4
- sub_82B9DD48  game-only  len 53  pm4 [-]  engine call sites 4
- sub_82B9D970  game-only  len 5  pm4 [-]  engine call sites 4
- sub_82B991B0  game-only  len 21  pm4 [-]  engine call sites 4
- sub_82B99208  game-only  len 21  pm4 [-]  engine call sites 4
- sub_82B9D958  game-only  len 5  pm4 [-]  engine call sites 3
- sub_82B9D840  game-only  len 23  pm4 [-]  engine call sites 3
- sub_82B9D4F8  game-only  len 7  pm4 [-]  engine call sites 3
- sub_82B9D4E0  game-only  len 5  pm4 [-]  engine call sites 3
- sub_82B9D988  game-only  len 1  pm4 [-]  engine call sites 3
- sub_82BA6C18  game-only  len 168  pm4 [-]  engine call sites 3
- sub_82B9D8F8  game-only  len 23  pm4 [-]  engine call sites 3
- sub_82B9D638  game-only  len 31  pm4 [-]  engine call sites 3
- sub_82B9DA58  game-only  len 5  pm4 [-]  engine call sites 3
- sub_82BA2F68  game-only  len 119  pm4 [WAIT_REG_MEM]  engine call sites 3
- sub_82B9D7A8  game-only  len 38  pm4 [-]  engine call sites 3
- sub_82B9D990  game-only  len 50  pm4 [-]  engine call sites 3
- sub_82B9E750  game-only  len 93  pm4 [-]  engine call sites 3
- sub_82BA1FA8  game-only  len 54  pm4 [-]  engine call sites 3
- sub_82B9DB70  game-only  len 5  pm4 [-]  engine call sites 3
- sub_82B9D8A0  game-only  len 22  pm4 [-]  engine call sites 3
- sub_82BA1EE0  game-only  len 49  pm4 [-]  engine call sites 3
- sub_82B9E1F0  game-only  len 19  pm4 [-]  engine call sites 3
- sub_82B9F970  game-only  len 73  pm4 [-]  engine call sites 3
- sub_82BA34D8  game-only  len 457  pm4 [REG_RMW]  engine call sites 3
- sub_82B9D750  game-only  len 21  pm4 [-]  engine call sites 3
- sub_82BA2830  game-only  len 284  pm4 [-]  engine call sites 3
- sub_82BA49B0  game-only  len 81  pm4 [-]  engine call sites 2
- sub_82B9D540  game-only  len 30  pm4 [-]  engine call sites 2
- sub_82B9D518  game-only  len 10  pm4 [-]  engine call sites 2
- sub_82B9F940  game-only  len 5  pm4 [-]  engine call sites 2
- sub_82B9EE58  game-only  len 25  pm4 [-]  engine call sites 2
- sub_82B9D5B8  game-only  len 31  pm4 [-]  engine call sites 2
- sub_82B9D4C0  game-only  len 8  pm4 [-]  engine call sites 2
- sub_82BA40F0  game-only  len 98  pm4 [-]  engine call sites 2
- sub_82BA1A08  game-only  len 41  pm4 [-]  engine call sites 2
- sub_82B9DCB8  game-only  len 36  pm4 [-]  engine call sites 2
- sub_82BA2548  game-only  len 29  pm4 [-]  engine call sites 2
- sub_82BA22C0  game-only  len 2  pm4 [-]  engine call sites 2
- sub_82B9EEE0  game-only  len 85  pm4 [DRAW_INDX_2, INVALIDATE_STATE]  engine call sites 2
- sub_82B9EEC0  game-only  len 4  pm4 [-]  engine call sites 1
- sub_82BA1B78  game-only  len 49  pm4 [-]  engine call sites 1
- sub_82B997B0  game-only  len 4  pm4 [-]  engine call sites 1
- sub_82B9DBE8  game-only  len 29  pm4 [-]  engine call sites 1
- sub_82B994B8  game-only  len 60  pm4 [-]  engine call sites 1
- sub_82BA2270  shared  len 16  pm4 [-]  engine call sites 1
- sub_82BA1C40  game-only  len 63  pm4 [-]  engine call sites 1
- sub_82BA4678  game-only  len 80  pm4 [-]  engine call sites 1
- sub_82BA6508  game-only  len 176  pm4 [-]  engine call sites 1
- sub_82BA67C8  game-only  len 43  pm4 [-]  engine call sites 1
- sub_82BA3D88  shared  len 80  pm4 [-]  engine call sites 1
- sub_82BA2748  game-only  len 31  pm4 [-]  engine call sites 1
- sub_82B9F958  game-only  len 5  pm4 [-]  engine call sites 1
- sub_82B9DB88  game-only  len 6  pm4 [-]  engine call sites 1
- sub_82B9EED0  game-only  len 3  pm4 [-]  engine call sites 1
- sub_82B992B0  game-only  len 68  pm4 [-]  engine call sites 1
- sub_82B9DB08  game-only  len 26  pm4 [-]  engine call sites 1
- sub_82BA60C8  game-only  len 116  pm4 [REG_RMW]  engine call sites 1
- sub_82BA45E0  game-only  len 38  pm4 [-]  engine call sites 1
- sub_82BA6298  game-only  len 156  pm4 [-]  engine call sites 1
- sub_82BA6990  game-only  len 162  pm4 [-]  engine call sites 1
- sub_82B9F550  game-only  len 251  pm4 [-]  engine call sites 1
- sub_82BA2200  game-only  len 5  pm4 [-]  engine call sites 1
- sub_82B997A8  game-only  len 1  pm4 [-]  engine call sites 1
- sub_82BA4AF8  game-only  len 132  pm4 [-]  engine call sites 1
- sub_82B98F00  game-only  len 70  pm4 [-]  engine call sites 1
- sub_82BA22B0  game-only  len 4  pm4 [-]  engine call sites 1
- sub_82B9F3C0  game-only  len 19  pm4 [-]  engine call sites 1
- sub_82BA2500  game-only  len 17  pm4 [-]  engine call sites 1
- sub_82B9F038  game-only  len 226  pm4 [DRAW_INDX_2, IM_LOAD_IMMEDIATE, INVALIDATE_STATE]  engine call sites 1
- sub_82B993C0  game-only  len 62  pm4 [-]  engine call sites 1
- sub_82BA22C8  game-only  len 38  pm4 [-]  engine call sites 1
- sub_82BA2360  game-only  len 33  pm4 [-]  engine call sites 1
- sub_82B9E8C8  game-only  len 160  pm4 [-]  engine call sites 1

Total engine call sites into the library: 249
