#include "tu_cpu_profile.cc"
#include <cassert>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
#include <cstdio>
#include <cstdlib>

static std::mutex output_lock;
static std::vector<std::vector<TuCpuProfileRecord>> groups;
static void capture(const void *raw, uint32_t count, uint32_t size) {
   assert(size == sizeof(TuCpuProfileRecord));
   assert(count >= 1 && count <= TU_CPU_PHASE_COUNT);
   const auto *records = static_cast<const TuCpuProfileRecord *>(raw);
   assert(records[0].phase == TU_CPU_SUMMARY && records[0].count + 1 == count);
   for (uint32_t i = 0; i < count; ++i) {
      assert(records[i].group == records[0].group);
      assert(records[i].command == records[0].command);
      assert(records[i].thread == records[0].thread);
      assert(records[i].version == 1);
   }
   std::lock_guard<std::mutex> held(output_lock);
   groups.emplace_back(records, records + count);
}
static void sleep_ms(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static void consume_cpu(unsigned ms) {
   uint64_t begin = 0, now = 0;
   assert(cpu_ns(begin));
   do { assert(cpu_ns(now)); } while (now - begin < uint64_t(ms) * 1000000);
}
static const TuCpuProfileRecord &phase(const std::vector<TuCpuProfileRecord> &rows, uint32_t id) {
   for (const auto &row : rows) if (row.phase == id) return row;
   std::abort();
}
int main() {
   { TuCpuCall call(TU_CPU_BEGIN_PASS, 1); TuCpuPhase p(TU_CPU_ATTACHMENTS); sleep_ms(10); }
   assert(groups.empty());
   tu_cpu_profile_set_hook(capture);
   {
      TuCpuCall call(TU_CPU_BEGIN_PASS, 0x123456789abcdef0ull);
      TuCpuPhase parent(TU_CPU_SUBPASS_EMIT);
      sleep_ms(10);
      { TuCpuPhase child(TU_CPU_CS_RESERVE); sleep_ms(12); }
   }
   assert(groups.size() == 1);
   const auto &sleep = groups[0];
   assert(sleep[0].command == 0x123456789abcdef0ull && sleep[0].wall_ns >= 22000000);
   assert(sleep[0].flags & 1);
   const auto &parent = phase(sleep, TU_CPU_SUBPASS_EMIT);
   const auto &child = phase(sleep, TU_CPU_CS_RESERVE);
   assert(parent.wall_ns >= child.wall_ns && parent.count == child.count);
   assert(parent.self_ns + child.self_ns == parent.wall_ns);
   assert(sleep[0].self_ns + parent.self_ns + child.self_ns == sleep[0].wall_ns);
   {
      TuCpuCall call(TU_CPU_DRAW, 2);
      TuCpuPhase p(TU_CPU_DRAW_STATE);
      consume_cpu(16);
   }
   assert(groups.size() == 2 && groups[1][0].cpu_ns >= 16000000);
   assert(groups[1][0].flags & 1);
   {
      TuCpuCall outer(TU_CPU_END_PASS, 3);
      TuCpuPhase p(TU_CPU_RENDER);
      { TuCpuCall nested(TU_CPU_BIND_PIPELINE, 4); sleep_ms(10); }
   }
   assert(groups.size() == 3 && groups[2][0].command == 3);
   std::thread other([] { TuCpuCall c(TU_CPU_UPDATE_BUFFER, 5); TuCpuPhase p(TU_CPU_BO_INIT); sleep_ms(10); });
   other.join();
   assert(groups.size() == 4 && groups[3][0].thread != groups[0][0].thread);
   profile_written.store(max_groups - 1);
   { TuCpuCall call(TU_CPU_BIND_PIPELINE, 7); sleep_ms(10); }
   assert(groups.size() == 5 && (groups.back()[0].flags & 8));
   { TuCpuCall call(TU_CPU_BEGIN_PASS, 8); sleep_ms(10); }
   assert(groups.size() == 5);
   tu_cpu_profile_set_hook(nullptr);
   { TuCpuCall call(TU_CPU_DRAW, 6); sleep_ms(10); }
   assert(groups.size() == 5);
   std::puts("PASS: real profiler disabled-hook, nested exclusive time, sleep/CPU distinction, nested API, thread identity and callback layout");
   for (const auto &rows : groups)
      std::printf("api=%u wall_ns=%llu cpu_ns=%llu phases=%u flags=%u\n", rows[0].api,
         (unsigned long long)rows[0].wall_ns, (unsigned long long)rows[0].cpu_ns,
         rows[0].count, rows[0].flags);
}
