## BVH Debug

1. `INTEL_DEBUG=bvh_tlas,bvh_blas` will generate `tlas_{id}.txt` or `blas_{id}.txt` in `bvh_dump/BVH_ANV` directory.
2. `INTEL_DEBUG=bvh_tlas_ir_hdr,bvh_blas_ir_hdr` will generate `tlas_{id}.txt` or `blas_{id}.txt` in `bvh_dump/BVH_IR_HDR` directory.
3. `INTEL_DEBUG=bvh_tlas_ir_as,bvh_blas_ir_as` will generate `tlas_{id}.txt` or `blas_{id}.txt` in `bvh_dump/BVH_IR_AS` directory.
4. `INTEL_DEBUG=bvh_no_build` will skip the intel-specific-encoding part. If gpu hang is seen, this is the first step to isolate the problem. If toggled on and gpu doesn't hang anymore, that means it was either encode.comp was spinning, or the built bvh has issues so gpu hung during bvh traversal.

The dumped text file contains memory dump, byte-by-byte in hex. The contents are contiguous memory of a certain region.
1. The dump in `BVH_ANV` starts from the beginning of `anv_accel_struct_header` to the end of the bvh. Nodes/leaves are packed tightly after the header, encoded in a way that our HW expects.
2. The dump in `BVH_IR_HDR` records the contents of `vk_ir_header` sitting at the beginning of ir bvh.
3. The dump in `BVH_IR_AS` records all `vk_ir_{leaf_type}_node` and  `vk_ir_box_node` in ir bvh. The region starts from where leaves are encoded to the end of ir bvh.

### Decode the dump

We have a way to decode the dump in `BVH_ANV`.
- To decode this memory dump, use a python script to parse the file and generate a human-readable json.
- To further visualize the tree, there is a html that parses the json and draws the tree topology and 3D views of bounding boxes.

```
# Using blas_0 as an example
xxd -r -p bvh_dump/BVH_ANV/blas_0.txt > input.bin

# Use a python script to generate a human-readable json file
cd mesa/src/intel/vulkan/bvh/
python3 interpret.py <path/to/input.bin>

# To further visualize the tree, the html parses the json and draws it in 3D.
cd mesa/src/intel/vulkan/bvh/
python3 -m http.server 8000
# go to localhost:8000/visualize_json.html

```

### Note and Limitations:
1. The python script use `ctypes` to interpret the memory dump, so the structure defined in the script should match the structure defined in the driver.
2. The memory dump is a snapshot of a VkBuffer captured at the end of `CmdBuildAccelerationStructure` call. It won't capture any bvh obtained from `CmdCopy`.
3. The memory dump of captured bvhs so far are saved to files at the moment when `DestroyAccelerationStructure` is called every time.
4. If ANY dump is enabled, we will nullify anv tlas bvh and send all 0s to the gpu. Doing this can prevent gpu hang caused by incorrect bvh traversal. However, the actual contents are still saved to files for debugging.
