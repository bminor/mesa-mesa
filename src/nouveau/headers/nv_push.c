#include "nv_push.h"

#include "nv_device_info.h"

#include <inttypes.h>
#include "util/os_misc.h"

#include "nv_push_class_dump.h"

#ifndef NDEBUG
void
nv_push_validate(struct nv_push *push)
{
   uint32_t *cur = push->start;

   /* submitting empty push buffers is probably a bug */
   assert(push->end != push->start);

   /* make sure we don't overrun the bo */
   assert(push->end <= push->limit);

   /* parse all the headers to see if we get to buf->map */
   while (cur < push->end) {
      uint32_t hdr = *cur;
      uint32_t mthd = hdr >> 29;

      switch (mthd) {
      /* immd */
      case 4:
         break;
      case 1:
      case 3:
      case 5: {
         uint32_t count = (hdr >> 16) & 0x1fff;
         assert(count);
         cur += count;
         break;
      }
      default:
         assert(!"unknown method found");
      }

      cur++;
      assert(cur <= push->end);
   }
}
#endif

void
vk_push_print(FILE *fp, const struct nv_push *push,
              const struct nv_device_info *devinfo)
{
   uint32_t *cur = push->start;
   uint16_t curr_subchans[8] = {0};
   curr_subchans[0] = devinfo->cls_eng3d;
   curr_subchans[1] = devinfo->cls_compute;
   curr_subchans[2] = devinfo->cls_m2mf;
   curr_subchans[3] = 0x2d;
   curr_subchans[4] = devinfo->cls_copy;

   const bool print_offsets = true;

   while (cur < push->end) {
      uint32_t hdr = *cur;
      uint32_t type = hdr >> 29;
      bool is_tert = type == 0 || type == 2;
      uint32_t inc = 0;
      uint32_t count = is_tert ? (hdr >> 18) & 0x3ff : (hdr >> 16) & 0x1fff;
      uint32_t tert_op = (hdr >> 16) & 0x3;
      uint32_t subchan = (hdr >> 13) & 0x7;
      uint32_t mthd = (hdr & 0xfff) << 2;
      uint32_t value = 0;
      bool is_immd = false;

      if (print_offsets)
         fprintf(fp, "[0x%08" PRIxPTR "] ", cur - push->start);

      if (is_tert && tert_op != 0) {
         fprintf(fp, "HDR %x subch N/A", hdr);
      } else {
         fprintf(fp, "HDR %x subch %i", hdr, subchan);
      }

      cur++;

      const char *mthd_name = "";

      switch (type) {
      case 4:
         fprintf(fp, " IMMD\n");
         inc = 0;
         is_immd = true;
         value = count;
         count = 1;
         break;
      case 1:
         fprintf(fp, " NINC\n");
         inc = count;
         break;
      case 2:
      case 3:
         fprintf(fp, " 0INC\n");
         inc = 0;
         break;
      case 5:
         fprintf(fp, " 1INC\n");
         inc = 1;
         break;
      case 0:
         switch (tert_op) {
         case 0:
            fprintf(fp, " NINC\n");
            inc = count;
            break;
         case 1:
            fprintf(fp, " SUB_DEVICE_OP\n");
            mthd_name = "SET_SUBDEVICE_MASK";
            mthd = tert_op;
            value = (hdr >> 4) & 0xfff;
            count = 1;
            is_immd = true;
            break;
         case 2:
            fprintf(fp, " SUB_DEVICE_OP\n");
            mthd_name = "STORE_SUBDEVICE_MASK";
            mthd = tert_op;
            value = (hdr >> 4) & 0xfff;
            count = 1;
            is_immd = true;
            break;
         case 3:
            fprintf(fp, " SUB_DEVICE_OP\n");
            mthd_name = "USE_SUBDEVICE_MASK";
            mthd = tert_op;
            count = 1;
            break;
         }
         break;
      }

      while (count--) {
         if (!is_immd)
            value = *cur;

         if (mthd == 0) { /* SET_OBJECT */
            curr_subchans[subchan] = value & 0xffff;
         }
         int class_id = curr_subchans[subchan];

         if (!is_tert)
            mthd_name = P_PARSE_NV_MTHD(class_id, mthd);

         fprintf(fp, "\tmthd %04x %s\n", mthd, mthd_name);
         P_DUMP_NV_MTHD_DATA(fp, class_id, mthd, value, "\t\t");

         if (!is_immd)
            cur++;

         if (inc) {
            inc--;
            mthd += 4;
         }
      }

      fprintf(fp, "\n");
   }
}
