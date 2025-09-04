#include "nv_push.h"

#include "nv_device_info.h"

#include <inttypes.h>
#include "util/os_misc.h"

#include "nv_push_cl902d.h"
#include "nv_push_cl9039.h"
#include "nv_push_cl906f.h"
#include "nv_push_cl9097.h"
#include "nv_push_cl90b5.h"
#include "nv_push_cla097.h"
#include "nv_push_cla0b5.h"
#include "nv_push_cla040.h"
#include "nv_push_cla0c0.h"
#include "nv_push_cla140.h"
#include "nv_push_clb06f.h"
#include "nv_push_clb197.h"
#include "nv_push_clc0c0.h"
#include "nv_push_clc1b5.h"
#include "nv_push_clc397.h"
#include "nv_push_clc3c0.h"
#include "nv_push_clc56f.h"
#include "nv_push_clc597.h"
#include "nv_push_clc5b5.h"
#include "nv_push_clc5c0.h"
#include "nv_push_clc5b0.h"
#include "nv_push_clc697.h"
#include "nv_push_clc6b5.h"
#include "nv_push_clc6c0.h"
#include "nv_push_clc797.h"
#include "nv_push_clc7c0.h"
#include "nv_push_clcab5.h"

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
         int cls_hi = (class_id & 0xff00) >> 8;
         int cls_lo = class_id & 0xff;

         if (!is_tert) {
            if (mthd < 0x100) {
               if (cls_hi >= 0xc5)
                  mthd_name = P_PARSE_NVC56F_MTHD(mthd);
               else if (cls_hi >= 0xb0)
                  mthd_name = P_PARSE_NVB06F_MTHD(mthd);
               else
                  mthd_name = P_PARSE_NV906F_MTHD(mthd);
            } else {
               switch (cls_lo) {
               case 0x97:
                  if (cls_hi >= 0xc7)
                     mthd_name = P_PARSE_NVC797_MTHD(mthd);
                  else if (cls_hi >= 0xc6)
                     mthd_name = P_PARSE_NVC697_MTHD(mthd);
                  else if (cls_hi >= 0xc5)
                     mthd_name = P_PARSE_NVC597_MTHD(mthd);
                  else if (cls_hi >= 0xc3)
                     mthd_name = P_PARSE_NVC397_MTHD(mthd);
                  else if (cls_hi >= 0xb1)
                     mthd_name = P_PARSE_NVB197_MTHD(mthd);
                  else if (cls_hi >= 0xa0)
                     mthd_name = P_PARSE_NVA097_MTHD(mthd);
                  else
                     mthd_name = P_PARSE_NV9097_MTHD(mthd);
                  break;
               case 0xc0:
                  if (cls_hi >= 0xc7)
                     mthd_name = P_PARSE_NVC7C0_MTHD(mthd);
                  else if (cls_hi >= 0xc6)
                     mthd_name = P_PARSE_NVC6C0_MTHD(mthd);
                  else if (cls_hi >= 0xc5)
                     mthd_name = P_PARSE_NVC5C0_MTHD(mthd);
                  else if (cls_hi >= 0xc3)
                     mthd_name = P_PARSE_NVC3C0_MTHD(mthd);
                  else if (cls_hi >= 0xc0)
                     mthd_name = P_PARSE_NVC0C0_MTHD(mthd);
                  else
                     mthd_name = P_PARSE_NVA0C0_MTHD(mthd);
                  break;
               case 0x39:
               case 0x40:
                  if (cls_hi >= 0xa1)
                     mthd_name = P_PARSE_NVA140_MTHD(mthd);
                  else if (cls_hi >= 0xa0)
                     mthd_name = P_PARSE_NVA040_MTHD(mthd);
                  else if (cls_hi >= 0x90)
                     mthd_name = P_PARSE_NV9039_MTHD(mthd);
                  break;
               case 0x2d:
                  mthd_name = P_PARSE_NV902D_MTHD(mthd);
                  break;
               case 0xb5:
                  if (cls_hi >= 0xca)
                     mthd_name = P_PARSE_NVCAB5_MTHD(mthd);
                  else if (cls_hi >= 0xc6)
                     mthd_name = P_PARSE_NVC6B5_MTHD(mthd);
                  else if (cls_hi >= 0xc5)
                     mthd_name = P_PARSE_NVC5B5_MTHD(mthd);
                  else if (cls_hi >= 0xc1)
                     mthd_name = P_PARSE_NVC1B5_MTHD(mthd);
                  else if (cls_hi >= 0xa0)
                     mthd_name = P_PARSE_NVA0B5_MTHD(mthd);
                  else
                     mthd_name = P_PARSE_NV90B5_MTHD(mthd);
                  break;
               case 0xb0:
                  mthd_name = P_PARSE_NVC5B0_MTHD(mthd);
                  break;
               default:
                  mthd_name = "unknown method";
                  break;
               }
            }
         }

         fprintf(fp, "\tmthd %04x %s\n", mthd, mthd_name);
         if (mthd < 0x100) {
            if (cls_hi >= 0xb0)
               P_DUMP_NVB06F_MTHD_DATA(fp, mthd, value, "\t\t");
            else
               P_DUMP_NV906F_MTHD_DATA(fp, mthd, value, "\t\t");
         } else {
            switch (cls_lo) {
            case 0x97:
               if (cls_hi >= 0xc5)
                  P_DUMP_NVC597_MTHD_DATA(fp, mthd, value, "\t\t");
               else if (cls_hi >= 0xc3)
                  P_DUMP_NVC397_MTHD_DATA(fp, mthd, value, "\t\t");
               else if (cls_hi >= 0xb1)
                  P_DUMP_NVB197_MTHD_DATA(fp, mthd, value, "\t\t");
               else if (cls_hi >= 0xa0)
                  P_DUMP_NVA097_MTHD_DATA(fp, mthd, value, "\t\t");
               else
                  P_DUMP_NV9097_MTHD_DATA(fp, mthd, value, "\t\t");
               break;
            case 0xc0:
               if (cls_hi >= 0xc3)
                  P_DUMP_NVC3C0_MTHD_DATA(fp, mthd, value, "\t\t");
               else if (cls_hi >= 0xc0)
                  P_DUMP_NVC0C0_MTHD_DATA(fp, mthd, value, "\t\t");
               else
                  P_DUMP_NVA0C0_MTHD_DATA(fp, mthd, value, "\t\t");
               break;
            case 0x39:
            case 0x40:
               if (cls_hi >= 0xa1)
                  P_DUMP_NVA140_MTHD_DATA(fp, mthd, value, "\t\t");
               else if (cls_hi >= 0xa0)
                  P_DUMP_NVA040_MTHD_DATA(fp, mthd, value, "\t\t");
               else if (cls_hi >= 0x90)
                  P_DUMP_NV9039_MTHD_DATA(fp, mthd, value, "\t\t");
               break;
            case 0x2d:
               P_DUMP_NV902D_MTHD_DATA(fp, mthd, value, "\t\t");
               break;
            case 0xb5:
               if (cls_hi >= 0xca)
                  P_DUMP_NVCAB5_MTHD_DATA(fp, mthd, value, "\t\t");
               else if (cls_hi >= 0xc1)
                  P_DUMP_NVC1B5_MTHD_DATA(fp, mthd, value, "\t\t");
               else if (cls_hi >= 0xa0)
                  P_DUMP_NVA0B5_MTHD_DATA(fp, mthd, value, "\t\t");
               else
                  P_DUMP_NV90B5_MTHD_DATA(fp, mthd, value, "\t\t");
               break;
            case 0xb0:
                  P_DUMP_NVC5B0_MTHD_DATA(fp, mthd, value, "\t\t");
                  break;
            default:
               fprintf(fp, "%s.VALUE = 0x%x\n", "\t\t", value);
               break;
            }
         }

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
