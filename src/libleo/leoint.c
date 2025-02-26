#include <ultra64.h>
#include "libleo/internal.h"

extern vu16 LEOrw_flags;
extern u8 LEOC2_Syndrome[2][0xE8*4];
extern block_param_form LEOc2_param;

u32 read_write_track();
u32 leoChk_mecha_int(void);
void leosetup_BM(void);
u32 leochk_err_reg(void);

void leointerrupt(void* arg) {
    u32 result;
    u32 tg_blocks;

    osCreateMesgQueue(&LEOc2ctrl_que, (OSMesg)&LEOc2ctrl_que_buf, 1);

    while (true) {
        osStopThread(&LEOinterruptThread);
        tg_blocks = LEOcur_command->data.readwrite.xfer_blks;
        LEOwrite_pointer = LEOcur_command->data.readwrite.buff_ptr;
        do {
            leoLba_to_phys(LEOtgt_param.lba);
            if (LEOrw_flags & 0x8000) {
                result = leoSeek_i(1);
            } else {
                result = leoSeek_i(0);
            }

            if (result != 0) {
                goto complete;
            }

            if (LEOrw_flags & 0x2000) {
                LEOtgt_param.rdwr_blocks = 1;
            } else if (LEOtgt_param.rdwr_blocks > tg_blocks) {
                LEOtgt_param.rdwr_blocks = tg_blocks;
            }
            LEOtgt_param.lba += LEOtgt_param.rdwr_blocks;
            tg_blocks -= LEOtgt_param.rdwr_blocks;
            result = read_write_track();
            if (result != 0) {
                goto complete;
            }

            LEOcur_command->data.readwrite.rw_bytes = LEOwrite_pointer - (u8*)LEOcur_command->data.readwrite.buff_ptr;
        } while (tg_blocks != 0);
        result = 0x90000; // Inaccessible?

    complete:;

        osSendMesg(&LEOcontrol_que, result, 1);
    }
}

u32 read_write_track(void) {
    u32 message;
    u32 block;
    u32 retry_cntr;
    block_param_form block_param;
    u16 bnum;
    u8* temp;
    u32 c2datasize;
    
    block_param.bytes = LEOtgt_param.sec_bytes;
    block_param.blkbytes = LEOtgt_param.blk_bytes;
    if (LEOrw_flags & 0x2000) {
        //Sector Mode
        block_param.blkbytes = block_param.bytes;
    }
    block_param.pntr = LEOwrite_pointer;
    LEOwrite_pointer += block_param.blkbytes;
    if (LEOtgt_param.rdwr_blocks == 2) {
        LEOwrite_pointer += block_param.blkbytes;
    }
    retry_cntr = 0;
    for (;;) {
        LEOPiInfo->transferInfo.transferMode = 1;
        LEOPiInfo->transferInfo.blockNum = 0;
        LEOPiInfo->transferInfo.block[0].C1ErrNum = 0;
        LEOPiInfo->transferInfo.block[0].sectorSize = block_param.bytes;
        LEOPiInfo->transferInfo.block[0].dramAddr = block_param.pntr;
        LEOPiInfo->transferInfo.block[0].C2Addr = &LEOC2_Syndrome[0];
        if (LEOrw_flags & 0x2000) {
            //Sector Mode
            LEOtgt_param.rdwr_blocks = 1;
            LEOPiInfo->transferInfo.transferMode = 3;
        } else if (LEOtgt_param.rdwr_blocks == 2) {
            LEOPiInfo->transferInfo.transferMode = 2;
            LEOPiInfo->transferInfo.block[1] = LEOPiInfo->transferInfo.block[0];
            LEOPiInfo->transferInfo.block[1].C2Addr = &LEOC2_Syndrome[1];
            LEOPiInfo->transferInfo.block[1].dramAddr = ((u8*)LEOPiInfo->transferInfo.block[1].dramAddr + block_param.blkbytes);
        }
        message = leoChk_mecha_int();
        if (message == 0) {
            if (LEOrw_flags & 0x8000) {
                //Write Mode
                leoSet_mseq(1);
            } else {
                //Read Mode
                leoSet_mseq(0);
            }
            leosetup_BM();
            LEOPiInfo->transferInfo.bmCtlShadow = LEOasic_bm_ctl_shadow;
            LEOPiInfo->transferInfo.seqCtlShadow = LEOasic_seq_ctl_shadow;
            if (LEOrw_flags & 0x8000) {
                LEOPiInfo->transferInfo.cmdType = 1;
                osWritebackDCache(block_param.pntr, block_param.blkbytes * LEOtgt_param.rdwr_blocks);
                osEPiStartDma(LEOPiInfo, &LEOPiDmaParam, 1);
                osRecvMesg(&LEOdma_que, NULL, 1);
                LEOasic_bm_ctl_shadow = LEOPiInfo->transferInfo.bmCtlShadow;
                LEOasic_seq_ctl_shadow = LEOPiInfo->transferInfo.seqCtlShadow;
                bnum = LEOPiInfo->transferInfo.blockNum;
                message = LEOPiInfo->transferInfo.block[bnum].errStatus;
                if (message == 0) {
                    return message;
                }
                goto track_end;
            } else {
                if (LEOrw_flags & 0x4000) {
                    osRecvMesg(&LEOc2ctrl_que, NULL, 1);
                    osSendMesg(&LEOc2ctrl_que, NULL, 0);
                }
                LEOPiInfo->transferInfo.cmdType = 0;
                osInvalDCache(block_param.pntr, block_param.blkbytes * LEOtgt_param.rdwr_blocks);
                osEPiStartDma(LEOPiInfo, &LEOPiDmaParam, 0);
                block = 0;
                while (LEOtgt_param.rdwr_blocks != 0) {
                    osRecvMesg(&LEOdma_que, NULL, 1);
                    LEOasic_bm_ctl_shadow = LEOPiInfo->transferInfo.bmCtlShadow;
                    LEOasic_seq_ctl_shadow = LEOPiInfo->transferInfo.seqCtlShadow;
                    message = LEOPiInfo->transferInfo.block[block].errStatus;
                    if (message != 0) {
                        goto track_end;
                    }
                    if (LEOrw_flags & 0x2000) {
                        return 0;
                    }
                    if (LEOPiInfo->transferInfo.block[block].C1ErrNum) {
                        if (LEOPiInfo->transferInfo.block[block].C1ErrSector[0] < 0x55) {
                            if (LEOtgt_param.rdwr_blocks == 1) {
                                osEPiReadIo(LEOPiInfo, 0x05000514, &message);
                                if (message & 0x10000000) {
                                    message = 4;
                                    goto track_end;
                                }
                            }
                            
                            if (block == 0) {
                                temp = LEOC2_Syndrome[0];
                            } else {
                                temp = LEOC2_Syndrome[1];
                            }
                            c2datasize = block_param.bytes * 4;
                            block_param.c2buff_e = temp + c2datasize;
                            osInvalDCache(temp, c2datasize);
                            block_param.err_num = LEOPiInfo->transferInfo.block[block].C1ErrNum;
                            block_param.err_pos[0] = LEOPiInfo->transferInfo.block[block].C1ErrSector[0];
                            block_param.err_pos[1] = LEOPiInfo->transferInfo.block[block].C1ErrSector[1];
                            block_param.err_pos[2] = LEOPiInfo->transferInfo.block[block].C1ErrSector[2];
                            block_param.err_pos[3] = LEOPiInfo->transferInfo.block[block].C1ErrSector[3];
                            osRecvMesg(&LEOc2ctrl_que, NULL, 1);
                            LEOrw_flags |= 0x4000;
                            LEOc2_param = block_param;
                            osSendMesg(&LEOcontrol_que, (void* )0x80000, 1);
                        }
                    } else {
                        if (LEOtgt_param.rdwr_blocks == 1) {
                            if ((*(u32*)&LEOC2_Syndrome[block][0x00] | *(u32*)&LEOC2_Syndrome[block][0x04] | *(u32*)&LEOC2_Syndrome[block][0x08] | *(u32*)&LEOC2_Syndrome[block][0x0C]) != 0) {
                                message = 0x17;
                                goto track_end;
                            }
                        }
                    }
                    block++;
                    block_param.pntr += block_param.blkbytes;
                    LEOtgt_param.start_block ^= 1;
                    LEOtgt_param.rdwr_blocks--;
                }
            }
            return 0;
        }
track_end:
        if (message == 0x16) {
            message = leochk_err_reg();
        }
do_retry:
        if (leoChk_err_retry(message) || (LEOrw_flags & 0x1000) || retry_cntr++ == 0x40) {
            break;
        }
        if ((retry_cntr & 7) == 0) {
            message = leoSend_asic_cmd_w(0x30001, 0);
            if (message != 0) {
                goto do_retry;
            }
        }
        if (message == 0x18 || (message == 0x17 && retry_cntr == 0x20)) {
            message = leoDetect_index_w();
            if (message != 0) {
                goto do_retry;
            }
        }
        if (LEOrw_flags & 0x8000) {
            message = leoSeek_i(1);
        } else {
            message = leoSeek_i(0);
        }
        if (message != 0) {
            goto do_retry;
        }
    }
    return message;
}

u32 leoChk_mecha_int(void) {
    u32 stat = leoWait_mecha_cmd_done(ASIC_RD_SEEK);
    u32 index_stat;

    if (stat == 0) {
        osEPiReadIo(LEOPiInfo, LEO_CUR_TK, &index_stat);
        if ((index_stat & LEO_CUR_TK_INDEX_LOCK) != LEO_CUR_TK_INDEX_LOCK) {
            stat = 0x18;
        }
    }
    return stat;
}

void leosetup_BM(void) {
    osEPiWriteIo(LEOPiInfo, 0x05000510U, LEOasic_bm_ctl_shadow | 0x10000000);
    osEPiWriteIo(LEOPiInfo, 0x05000510U, LEOasic_bm_ctl_shadow);
    if (LEOtgt_param.start_block != 0) {
        LEOasic_bm_ctl_shadow = 0x5A0000;
    } else {
        LEOasic_bm_ctl_shadow = 0;
    }
    if (!(LEOrw_flags & 0x8000)) {
        LEOasic_bm_ctl_shadow |= 0x40000000;
    }
    if (LEOtgt_param.rdwr_blocks == 2) {
        LEOasic_bm_ctl_shadow |= 0x02000000;
    }
    osEPiWriteIo(LEOPiInfo, 0x05000510U, LEOasic_bm_ctl_shadow);
}

u32 leochk_err_reg(void) {
    u32 sense;
    u32 index_status;

    osEPiReadIo(LEOPiInfo, 0x05000514U, &sense);
    osEPiWriteIo(LEOPiInfo, 0x05000510U, LEOasic_bm_ctl_shadow | 0x10000000);
    osEPiWriteIo(LEOPiInfo, 0x05000510U, LEOasic_bm_ctl_shadow);
    if (sense & 0x04000000) {
        return 0x31;
    }
    if (sense & 0x10000000) {
        return 4;
    }
    if (sense & 0x42000000) {
        if (LEOrw_flags & 0x8000) {
            return 0x16;
        }
        return 0x17;
    }
    if (sense & 0x80000000) {
        return 0x18;
    }
    osEPiReadIo(LEOPiInfo, 0x0500050CU, &index_status);
    if ((index_status & 0x60000000) == 0x60000000) {
        return 0x19;
    }
    return 0x18;
}
