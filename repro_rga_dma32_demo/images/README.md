# Demo Images

Place the test input image here using the filename expected by the demo:

`in0w1280-h720-rgba8888.bin`

The demo will read that file and write the result back into the same directory as:

`out0w1280-h720-rgba8888.bin`

Run the demo from `repro_rga_dma32_demo/build` so the relative path `../images` resolves correctly.

JPG/PNG mode:

- Input example: `../images/input.jpg`
- Output example: `../images/output.png`
- Run: `./rga_allocator_dma32_demo ../images/input.jpg ../images/output.png`

If the input or output path ends with `.jpg`, `.jpeg`, or `.png`, the demo will use OpenCV to decode/encode the image.
