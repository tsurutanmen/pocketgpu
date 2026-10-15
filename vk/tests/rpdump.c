// 落ちたときの手がかり（rp_dump_crash・rp_where_maps）の書き出しを、PC で確かめる試験（crash34）。
// server.c をそのまま取り込み（static の関数と変数を直接使う）、記録の再生の途中の表を本物の RP_BEGIN・RP_AB で埋めてから、
// 落ちたときと同じ書き出しを stderr に出す。Android の合図の受け手は、これと同じ rp_dump_crash を呼ぶ。
//   rpdump.exe          en（既定）
//   rpdump.exe ja       日本語
#include "../remote/server.c"

int main(int argc, char **argv) {
    g_lang_en = argc > 1 && !strcmp(argv[1], "ja") ? 0 : 1;
    static const uint32_t ops[] = { VR_CMD_BIND_PIPELINE, VR_CMD_BIND_DESCRIPTOR_SETS, VR_CMD_PUSH_CONSTANTS, VR_CMD_SET_VIEWPORT,
        VR_CMD_BIND_VERTEX_BUFFERS, VR_CMD_BIND_INDEX_BUFFER, VR_CMD_DRAW_INDEXED, VR_CMD_COPY_BUFFER_TO_IMAGE, VR_CMD_PIPELINE_BARRIER2,
        VR_CMD_BEGIN_RENDER_PASS, VR_CMD_DRAW, VR_CMD_END_RENDER_PASS, 9999 };
    // 1. 記録の再生の途中で落ちた、という表を作る（replay の中と同じ書き方）
    vr_rd fake = { NULL, 0, 0, 0 };
    vr_rd *r = &fake;
    g_cur_op = VR_RECORD_COMMAND_BUFFER;
    g_rp.cid = 777; g_rp.rec = 5; g_rp.len = 4096; g_rp.n = 0; g_rp.phase = 2;
    for (uint32_t k = 0; k < 20; k++) {
        r->at = 8 + k * 12;
        RP_BEGIN(ops[k % (sizeof ops / sizeof *ops)], r->at);
        RP_AB(1000 + k, ((uint64_t) k << 32) | (k * 2));
    }
    g_rp.gfx_pipe = 1234; g_rp.cmp_pipe = 5678;
    g_rpg_total = 3;
    printf("== dump (replay phase) ==\n"); fflush(stdout);
    rp_dump_crash();
    // 2. begin / end の最中に落ちた場合
    g_rp.phase = 3;
    printf("== dump (vkEndCommandBuffer phase) ==\n"); fflush(stdout);
    rp_dump_crash();
    // 3. 記録の再生と関係ない命令で落ちた場合は、何も書かない
    g_rp.phase = 0; g_cur_op = VR_QUEUE_SUBMIT;
    printf("== dump (other command: must print nothing) ==\n"); fflush(stdout);
    rp_dump_crash();
    // 4. maps の探し方
    static const char maps[] =
        "7000000000-7000100000 r--p 00000000 00:00 0                              [anon:x]\n"
        "7f12340000-7f12380000 r-xp 00011000 fd:01 1234  /vendor/lib64/hw/vulkan.adreno.so\n"
        "7f12380000-7f123c0000 rw-p 00055000 fd:01 1234  /vendor/lib64/hw/vulkan.adreno.so\n"
        "7f99000000-7f99001000 rw-p 00000000 00:00 0\n";
    printf("== maps ==\n"); fflush(stdout);
    rp_where_maps("  pc=", 0x7f12341234ull, maps, (int) sizeof maps - 1);
    rp_where_maps("  lr=", 0x7f99000800ull, maps, (int) sizeof maps - 1);
    rp_where_maps("  x=", 0x1234ull, maps, (int) sizeof maps - 1);
    printf("== done ==\n");
    return 0;
}
