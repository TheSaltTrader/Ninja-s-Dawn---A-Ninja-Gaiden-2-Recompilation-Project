# Native GPU M3 recon - ng2recomp

functions: 47799   shared-fingerprint with fable2recomp: 250

Direct3D library address range(s) (clusters holding the Vd* callers): 83734370-8374C728

engine functions that emit PM4 themselves (outside the library): 3

PM4 type-3 emitters: 24   Vd* callers: 15   library functions in range: 425 (shared 11)   API surface (library functions called by engine code): 81


## PM4 emitters (function, shared?, opcodes, #engine callers)

- sub_8373D570  game-only  len 914  [IM_LOAD_IMMEDIATE, INVALIDATE_STATE, WAIT_REG_MEM]  callers 44 (engine 43)
- sub_83734370  game-only  len 74  [SET_BIN_MASK_HI, SET_BIN_MASK_LO, SET_BIN_SELECT_LO]  callers 6 (engine 3)
- sub_8373E5B8  game-only  len 164  [WAIT_REG_MEM]  callers 5 (engine 3)
- sub_8374C5B8  game-only  len 91  [SET_BIN_MASK_LO]  callers 5 (engine 4)
- sub_83743268  game-only  len 95  [WAIT_REG_MEM]  callers 4 (engine 0)
- sub_8373A918  game-only  len 280  [ME_INIT]  callers 4 (engine 2)
- sub_83739CF0  game-only  len 106  [INDIRECT_BUFFER]  callers 3 (engine 0)
- sub_83748F18  game-only  len 116  [REG_RMW]  callers 1 (engine 0)
- sub_8373B060  game-only  len 102  [DRAW_INDX_2]  callers 1 (engine 0)
- sub_83738A38  game-only  len 420  [SET_BIN_MASK_LO]  callers 1 (engine 0)
- sub_8374BAC8  game-only  len 584  [EVENT_WRITE, IM_LOAD_IMMEDIATE, INVALIDATE_STATE]  callers 1 (engine 0)
- sub_83747940  game-only  len 155  [EVENT_WRITE_SHD]  callers 1 (engine 0)
- sub_83742C10  game-only  len 128  [EVENT_WRITE]  callers 1 (engine 0)
- sub_838554C8  game-only  len 77  [LOAD_ALU_CONSTANT]  callers 1 (engine 0)
- sub_83743F80  game-only  len 91  [IM_LOAD_IMMEDIATE, INVALIDATE_STATE]  callers 1 (engine 0)
- sub_83856420  game-only  len 77  [LOAD_ALU_CONSTANT]  callers 1 (engine 0)
- sub_8393C148  game-only  len 172  [REG_RMW]  callers 1 (engine 0)
- sub_83749AE0  game-only  len 55  [EVENT_WRITE_SHD]  callers 1 (engine 0)
- sub_83747BB0  game-only  len 46  [SET_BIN_SELECT_LO]  callers 1 (engine 0)
- sub_83740290  game-only  len 1410  [REG_TO_MEM]  callers 1 (engine 0)
- sub_8373F928  game-only  len 56  [COND_WRITE]  callers 1 (engine 0)
- sub_8373A088  game-only  len 58  [EVENT_WRITE_SHD]  callers 1 (engine 0)
- sub_8374C728  game-only  len 220  [EVENT_WRITE]  callers 1 (engine 1)
- sub_8373FA08  game-only  len 62  [COND_WRITE]  callers 1 (engine 0)

## Vd* kernel callers

- sub_8373A918  game-only  VdEnableRingBufferRPtrWriteBack, VdInitializeRingBuffer, VdSetSystemCommandBufferGpuIdentifierAddress  callers 4
- sub_8373F498  game-only  VdGetCurrentDisplayGamma  callers 2
- sub_83741B08  game-only  VdQueryVideoMode  callers 3
- sub_83741ED8  game-only  VdShutdownEngines  callers 0
- sub_83741F00  game-only  VdInitializeEngines, VdIsHSIOTrainingSucceeded, VdSetGraphicsInterruptCallback  callers 1
- sub_83742128  game-only  VdSetGraphicsInterruptCallback, VdSetSystemCommandBufferGpuIdentifierAddress, VdShutdownEngines  callers 1
- sub_837452C0  game-only  VdGetCurrentDisplayInformation, VdGetSystemCommandBuffer, VdPersistDisplay, VdSetDisplayMode, VdSwap  callers 3
- sub_83746F50  game-only  VdQueryVideoMode  callers 1
- sub_83747498  game-only  VdQueryVideoFlags  callers 2
- sub_837475C0  game-only  VdInitializeScalerCommandBuffer  callers 1
- sub_83747790  game-only  VdCallGraphicsNotificationRoutines  callers 2
- sub_83749BC0  game-only  VdEnableDisableClockGating  callers 1
- sub_8374B300  game-only  VdGetCurrentDisplayInformation  callers 1
- sub_8374B6A8  game-only  VdGetCurrentDisplayInformation  callers 4
- sub_8374C4D0  game-only  VdRetrainEDRAM, VdRetrainEDRAMWorker  callers 2

## API surface: library entry points called from engine code (sorted by call sites)

- sub_83736568  game-only  len 48  pm4 [-]  engine call sites 55
- sub_837363D8  game-only  len 48  pm4 [-]  engine call sites 55
- sub_8373BD50  game-only  len 296  pm4 [-]  engine call sites 49
- sub_8373D570  game-only  len 914  pm4 [IM_LOAD_IMMEDIATE, INVALIDATE_STATE, WAIT_REG_MEM]  engine call sites 43
- sub_8373E9E8  shared  len 32  pm4 [-]  engine call sites 40
- sub_8373B4D8  game-only  len 111  pm4 [-]  engine call sites 34
- sub_8373B880  game-only  len 115  pm4 [-]  engine call sites 32
- sub_837374E8  game-only  len 182  pm4 [-]  engine call sites 30
- sub_83737180  game-only  len 217  pm4 [-]  engine call sites 29
- sub_8373B1F8  game-only  len 58  pm4 [-]  engine call sites 26
- sub_837390C8  game-only  len 47  pm4 [-]  engine call sites 21
- sub_8373B3C8  game-only  len 68  pm4 [-]  engine call sites 20
- sub_8373E970  shared  len 29  pm4 [-]  engine call sites 20
- sub_8373B790  game-only  len 60  pm4 [-]  engine call sites 18
- sub_8373BC68  game-only  len 58  pm4 [-]  engine call sites 14
- sub_8373E848  game-only  len 73  pm4 [-]  engine call sites 13
- sub_8373BBE0  game-only  len 33  pm4 [-]  engine call sites 12
- sub_83736EC8  game-only  len 31  pm4 [-]  engine call sites 12
- sub_8373B2E0  game-only  len 58  pm4 [-]  engine call sites 10
- sub_837370F0  game-only  len 36  pm4 [-]  engine call sites 10
- sub_8373EA90  shared  len 35  pm4 [-]  engine call sites 8
- sub_83736F48  game-only  len 71  pm4 [-]  engine call sites 8
- sub_83737CB0  shared  len 21  pm4 [-]  engine call sites 7
- sub_8373CB08  game-only  len 263  pm4 [-]  engine call sites 6
- sub_8373A3D0  game-only  len 63  pm4 [-]  engine call sites 6
- sub_83736DC8  game-only  len 63  pm4 [-]  engine call sites 6
- sub_8373C1F0  game-only  len 332  pm4 [-]  engine call sites 4
- sub_8374C5B8  game-only  len 91  pm4 [SET_BIN_MASK_LO]  engine call sites 4
- sub_837369A8  game-only  len 27  pm4 [-]  engine call sites 4
- sub_83739360  game-only  len 69  pm4 [-]  engine call sites 4
- sub_8373A8C0  game-only  len 21  pm4 [-]  engine call sites 3
- sub_8373C720  game-only  len 250  pm4 [-]  engine call sites 3
- sub_8373E5B8  game-only  len 164  pm4 [WAIT_REG_MEM]  engine call sites 3
- sub_8373BAD8  game-only  len 5  pm4 [-]  engine call sites 3
- sub_83734370  game-only  len 74  pm4 [SET_BIN_MASK_HI, SET_BIN_MASK_LO, SET_BIN_SELECT_LO]  engine call sites 3
- sub_8373A7D0  game-only  len 30  pm4 [-]  engine call sites 2
- sub_83745B20  game-only  len 40  pm4 [-]  engine call sites 2
- sub_83737C98  game-only  len 5  pm4 [-]  engine call sites 2
- sub_83741DC0  game-only  len 39  pm4 [-]  engine call sites 2
- sub_83737D08  shared  len 16  pm4 [-]  engine call sites 2
- sub_8373EB20  game-only  len 50  pm4 [-]  engine call sites 2
- sub_8373A918  game-only  len 280  pm4 [ME_INIT]  engine call sites 2
- sub_83741B08  game-only  len 173  pm4 [-]  engine call sites 2
- sub_83737D48  game-only  len 4  pm4 [-]  engine call sites 2
- sub_83748C08  game-only  len 111  pm4 [-]  engine call sites 2
- sub_837459D8  game-only  len 82  pm4 [-]  engine call sites 2
- sub_83737068  game-only  len 34  pm4 [-]  engine call sites 2
- sub_83737950  game-only  len 51  pm4 [-]  engine call sites 2
- sub_837397E0  game-only  len 102  pm4 [-]  engine call sites 2
- sub_83744D90  game-only  len 106  pm4 [-]  engine call sites 1
- sub_83739318  game-only  len 17  pm4 [-]  engine call sites 1
- sub_8373ED50  game-only  len 59  pm4 [-]  engine call sites 1
- sub_83736290  game-only  len 21  pm4 [-]  engine call sites 1
- sub_83737DF0  game-only  len 34  pm4 [-]  engine call sites 1
- sub_83736908  game-only  len 24  pm4 [-]  engine call sites 1
- sub_8373EA68  game-only  len 10  pm4 [-]  engine call sites 1
- sub_83735CF8  shared  len 16  pm4 [-]  engine call sites 1
- sub_8373F180  game-only  len 49  pm4 [-]  engine call sites 1
- sub_83737BB8  game-only  len 55  pm4 [-]  engine call sites 1
- sub_8373FB10  game-only  len 9  pm4 [-]  engine call sites 1
- sub_8373ECF8  game-only  len 18  pm4 [-]  engine call sites 1
- sub_8373A4D0  game-only  len 53  pm4 [-]  engine call sites 1
- sub_837377C0  game-only  len 49  pm4 [-]  engine call sites 1
- sub_83737D58  game-only  len 38  pm4 [-]  engine call sites 1
- sub_83736A28  game-only  len 27  pm4 [-]  engine call sites 1
- sub_8373EE40  game-only  len 78  pm4 [-]  engine call sites 1
- sub_83734A78  game-only  len 7  pm4 [-]  engine call sites 1
- sub_83739188  game-only  len 73  pm4 [-]  engine call sites 1
- sub_83734B10  game-only  len 32  pm4 [-]  engine call sites 1
- sub_83748A18  game-only  len 124  pm4 [-]  engine call sites 1
- sub_8373A5A8  game-only  len 92  pm4 [-]  engine call sites 1
- sub_8373EC48  game-only  len 43  pm4 [-]  engine call sites 1
- sub_837452C0  game-only  len 454  pm4 [-]  engine call sites 1
- sub_8373ED40  game-only  len 3  pm4 [-]  engine call sites 1
- sub_83734810  game-only  len 153  pm4 [-]  engine call sites 1
- sub_8373EC38  game-only  len 4  pm4 [-]  engine call sites 1
- sub_8373FAF8  game-only  len 5  pm4 [-]  engine call sites 1
- sub_8373EBE8  game-only  len 20  pm4 [-]  engine call sites 1
- sub_837351D0  game-only  len 14  pm4 [-]  engine call sites 1
- sub_8373EF78  game-only  len 130  pm4 [-]  engine call sites 1
- sub_8374C728  game-only  len 220  pm4 [EVENT_WRITE]  engine call sites 1

Total engine call sites into the library: 675
