# Mini-AI <img align="left" width="42px" src=".github/logo.png" /> [![Github Build Status](https://github.com/turanszkij/mini-ai/workflows/Build/badge.svg)](https://github.com/turanszkij/mini-ai/actions)
A tiny graphical application for running simple AI generation tasks locally on your machine.<br/>
Made in C++ and WinAPI, no external dependencies.

<img width="24%" src=".github/image0.png" /> <img width="24%" src=".github/image1.png" /> <img width="24%" src=".github/image3.png" /> <img width="24%" src=".github/image2.png" />

## Features

 <img width="24%" src=".github/image4.png" align="right" />
 
- Image generation: You simply type a text and the image will be generated.
    - Z-Image Turbo
    - Flux 2 Klein
    - Stable Diffusion 3.5 Large
    - Qwen Image
    - ERNIE-Image Turbo
- Image Editing: Edit the current image with a prompt and other reference images
    - Flux 2 Klein
    - Qwen Image Edit
- Ask: enter a text and the AI will answer, also supports image if one is opened
    - Qwen 3 VL
    - Gemma 4
- Video: generate a video from text prompt, and/or from the reference images
    - Wan 2.2
    - LTX 2.3
- Copy-paste, undo-redo, resize, save, load, drag and drop, PNG, JPG, TGA, ICO support

## How to use
- Write a text prompt into the textbox and hit the bottom right button to generate image (or choose an other mode from its drop-down menu)
- On first use the required AI models will be downloaded from the internet for the currently selected mode into the `models/` folder, which can take long as they are often several GB large.
- Generated images and videos will be saved to the `output/` folder near the exe.
- You can resize the window to set your generation resolution by dragging the window bounds, or choosing from  resolution presets in the right-click menu.
- You can use Ctrl+C and Ctrl+V to copy images from/to the mini-ai application.
- You can create a seed.txt and write your random seed in there if you would like to fix the generation seed.
- To change image generation model, right click -> Image generation model... -> choose model
- You can save any image as `.ico` to use for a windows application icon, it will have multiple resolutions embedded in the file.

## How to build
- Run build.bat (requires Visual Studio C++ build tools to be installed)
 
## Third party libraries used
- [stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp)
- [llama.cpp](https://github.com/ggml-org/llama.cpp)
- [stb_image.h](https://github.com/nothings/stb/blob/master/stb_image.h)
- [stb_image_write.h](https://github.com/nothings/stb/blob/master/stb_image_write.h)
- [stb_image_resize2.h](https://github.com/nothings/stb/blob/master/stb_image_resize2.h)

## License
The MIT License (MIT)

Copyright (c) 2026 Turánszki János

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
