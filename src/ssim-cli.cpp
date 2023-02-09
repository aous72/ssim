/*
 * Copyright (c) 2021, Romain Bailly
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

#include <rmgr/ssim.h>
#if RMGR_SSIM_USE_OPENMP
    #include <rmgr/ssim-openmp.h>
#endif

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#ifdef _WIN32
    #include <tchar.h>
#else
    typedef char TCHAR;
    #define _T(s)     s
    #define _tmain    main
    #define _tcscmp   strcmp
    #define _tcsicmp  strcasecmp
    #define _tcsrchr  strrchr
    #define _tfopen   fopen
    #define _ftprintf fprintf
#endif


#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
RMGR_WARNING_PUSH()
RMGR_WARNING_GCC_DISABLE("-Wsign-compare")
RMGR_WARNING_GCC_DISABLE("-Wunused-but-set-variable")
#include "stb_image.h"
RMGR_WARNING_POP()


#define STB_IMAGE_WRITE_IMPLEMENTATION
RMGR_WARNING_PUSH()
RMGR_WARNING_GCC_DISABLE("-Wsign-compare")
#include "stb_image_write.h"
RMGR_WARNING_POP()


namespace
{

enum MapFormat
{
    MAP_FORMAT_BMP,
    MAP_FORMAT_PNG,
    MAP_FORMAT_TGA,
    MAP_FORMAT_PFM,
};

}


static void print_help(FILE* file)
{
    fprintf(file, "Usage: rmgr-ssim [options] img1 img2 [map]\n"
                  "Options:\n"
                  "  -#   Compute SSIM only for channel #\n"
                  "  -y   Compute SSIM on luminance, using BT.709\n"
                  "       For images with <= 2 channels, only channel 0's SSIM will be computed\n"
                  "       For images with >= 3 channels, first three channels are converted from RGB to Y\n"
                  "  -yuv Compute SSIM on yuv components, by converting RGB to YCbCr, using BT.601\n\n");
}


static stbi_uc* load_img(const TCHAR* path, int* width, int* height, int* channelCount)
{
    FILE* file = _tfopen(path, _T("rb"));
    if (file == NULL)
    {
        _ftprintf(stderr, _T("Failed to open file \"%s\"\n"), path);
        return NULL;
    }

    stbi_uc* img = stbi_load_from_file(file, width, height, channelCount, 0);
    if (img == NULL)
    {
        _ftprintf(stderr, _T("Failed to load image \"%s\":\n"), path);
        fprintf(stderr, "%s\n", stbi_failure_reason());
    }

    fclose(file);
    return img;
}

template<typename T>
static float compute_ssim(const T* img1, const T* img2, int width, int height, int imgChannelCount, int imgChannel, float* map, int mapChannelCount, int mapChannel)
{
    assert(imgChannel < imgChannelCount);
    assert(map == NULL || mapChannel < mapChannelCount);
    assert(map != NULL || mapChannel == 0);
    rmgr::ssim::Params params = {};
    params.width      = width;
    params.height     = height;
    params.imgA.init_interleaved(img1, width*imgChannelCount, imgChannelCount, imgChannel);
    params.imgB.init_interleaved(img2, width*imgChannelCount, imgChannelCount, imgChannel);
    params.ssimMap    = map + mapChannel;
    params.ssimStep   = mapChannelCount;
    params.ssimStride = width * mapChannelCount;
#if RMGR_SSIM_USE_OPENMP
    return rmgr::ssim::compute_ssim_openmp(params);
#else
    return rmgr::ssim::compute_ssim(params);
#endif
}


static int compute_ssims(const stbi_uc* img1, const stbi_uc* img2, int width, int height, int channelCount, int onlyChannel, bool luminance, bool convert_to_yuv, float* map, int mapChannelCount)
{
    if (channelCount < 3 && luminance)
        onlyChannel = 0;

    if (onlyChannel >= 0)
    {
        assert(map == NULL || mapChannelCount == 1);
        float ssim = compute_ssim(img1, img2, width, height, channelCount, onlyChannel, map, mapChannelCount, 0);
        if (rmgr::ssim::get_errno(ssim) != 0)
            return EXIT_FAILURE;
        printf("% 7.4f\n", ssim);
    }
    else if (luminance)
    {
        assert(map == NULL || mapChannelCount == 1);

        stbi_uc* lum1 = new stbi_uc[width * height];
        stbi_uc* lum2 = new stbi_uc[width * height];
        if (lum1 == NULL || lum2 == NULL)
        {
            delete[] lum1;
            delete[] lum2;
            fprintf(stderr, "Not enough memory\n");
            return EXIT_FAILURE;
        }

        // Constants from BT.709
        const unsigned rRatio = 13933; // 0.2126 * 65536
        const unsigned gRatio = 46871; // 0.7152 * 65536
        const unsigned bRatio =  4732; // 0.0722 * 65536

        // Convert to luminance
        const unsigned pixelCount = width * height;
        const stbi_uc* s1 = img1;
        const stbi_uc* s2 = img2;
        stbi_uc*       d1 = lum1;
        stbi_uc*       d2 = lum2;
        for (unsigned i=0; i<pixelCount; ++i)
        {
            const unsigned r1 = *s1++;
            const unsigned g1 = *s1++;
            const unsigned b1 = *s1++;
            const unsigned r2 = *s2++;
            const unsigned g2 = *s2++;
            const unsigned b2 = *s2++;
            *d1++ = static_cast<stbi_uc>((r1 * rRatio + g1 * gRatio + b1 * bRatio + 32768) / 65536);
            *d2++ = static_cast<stbi_uc>((r2 * rRatio + g2 * gRatio + b2 * bRatio + 32768) / 65536);
        }
        
        float ssim = compute_ssim(lum1, lum2, width, height, 1, 0, map, mapChannelCount, 0);
        delete[] lum2;
        delete[] lum1;

        if (rmgr::ssim::get_errno(ssim) != 0)
            return EXIT_FAILURE;
        printf("% 7.4f\n", ssim);
    }
    else if (convert_to_yuv)
    {
        assert(map == NULL || mapChannelCount == 3);

        float* lum1 = new float[width * height * channelCount];
        float* lum2 = new float[width * height * channelCount];
        if (lum1 == NULL || lum2 == NULL)
        {
            delete[] lum1;
            delete[] lum2;
            fprintf(stderr, "Not enough memory\n");
            return EXIT_FAILURE;
        }

        // Constants from BT.601
        const double dALPHA_RF = 0.299;
        const double dALPHA_GF = 0.587;
        const double dALPHA_BF = 0.114;
        const double dBETA_CbF = 0.5 / (1.0 - dALPHA_BF);
        const double dBETA_CrF = 0.5 / (1.0 - dALPHA_RF);
        const float ALPHA_RF = (float)dALPHA_RF;
        const float ALPHA_GF = (float)dALPHA_GF;
        const float ALPHA_BF = (float)dALPHA_BF;
        const float BETA_CbF = (float)dBETA_CbF;
        const float BETA_CrF = (float)dBETA_CrF;
        const float mid_point = 127.5f; // this is weird, but because the algorithm expects a dynamic range of
                                        // 0 to 255, the middle point becomes 127.5

        // Convert to luminance
        const unsigned pixelCount = width * height;
        const stbi_uc* s1 = img1;
        const stbi_uc* s2 = img2;
        float*         d1 = lum1;
        float*         d2 = lum2;
        float r, g, b, y, cb, cr;
        for (unsigned i=0; i<pixelCount; ++i)
        {
            r  = (float)*s1++;
            g  = (float)*s1++;
            b  = (float)*s1++;
            y  = ALPHA_RF * r + ALPHA_GF * g + ALPHA_BF * b;
            cb = BETA_CbF * (b - y) + mid_point;
            cr = BETA_CrF * (r - y) + mid_point;
            *d1++ = y;
            *d1++ = cb;
            *d1++ = cr;
            r  = (float)*s2++;
            g  = (float)*s2++;
            b  = (float)*s2++;
            y  = ALPHA_RF * r + ALPHA_GF * g + ALPHA_BF * b;
            cb = BETA_CbF * (b - y) + mid_point;
            cr = BETA_CrF * (r - y) + mid_point;
            *d2++ = y;
            *d2++ = cb;
            *d2++ = cr;
        }

        float sum = 0.0f, weighted_average = 0.0f;
        for (int c=0; c<channelCount; ++c)
        {
            float ssim = compute_ssim(lum1, lum2, width, height, channelCount, c, map, mapChannelCount, (map!=NULL)?c: 0);
            if (rmgr::ssim::get_errno(ssim) != 0)
                return EXIT_FAILURE;
            printf("Channel %u: % 7.4f\n", c, ssim);
            sum += ssim;
            if (c == 0)
                weighted_average += 0.8 * ssim;
            else if (c == 1 || c == 2)
                weighted_average += 0.1 * ssim;
        }
        printf("Average  : % 7.4f\n", sum/channelCount);
        printf("Weighted Average  : % 7.4f\n", weighted_average);

        delete[] lum2;
        delete[] lum1;
    }
    else
    {
        float average = 0.0f;
        for (int c=0; c<channelCount; ++c)
        {
            float ssim = compute_ssim(img1, img2, width, height, channelCount, c, map, mapChannelCount, (map!=NULL)?c: 0);
            if (rmgr::ssim::get_errno(ssim) != 0)
                return EXIT_FAILURE;
            printf("Channel %u: % 7.4f\n", c, ssim);
            average += ssim;
        }
        printf("Average  : % 7.4f\n", average/channelCount);
    }

    return EXIT_SUCCESS;
}


extern "C" int _tmain(int argc, TCHAR* argv[])
{
    if (argc == 2 && (_tcscmp(argv[1],_T("-h")) || _tcscmp(argv[1],_T("--help"))==0))
    {
        print_help(stdout);
        return EXIT_SUCCESS;
    }

    if (argc < 3 || argc > 5)
    {
        print_help(stderr);
        return EXIT_FAILURE;
    }

    int  onlyChannel = -1;
    bool luminance   = false;
    bool convert_to_yuv = false;
    int  filesIndex   = 1;

    // Parse options
    if (argc >= 4 && argv[1][0] == _T('-'))
    {
        const TCHAR* option = argv[1];
        if (_tcscmp(option, _T("-0")) == 0)
            onlyChannel     = 0;
        else if (_tcscmp(option, _T("-1")) == 0)
            onlyChannel     = 1;
        else if (_tcscmp(option, _T("-2")) == 0)
            onlyChannel     = 2;
        else if (_tcscmp(option, _T("-3")) == 0)
            onlyChannel     = 3;
        else if (_tcscmp(option, _T("-y")) == 0)
            luminance       = true;
        else if (_tcscmp(option, _T("-yuv")) == 0)
            convert_to_yuv  = true;
        else
        {
            _ftprintf(stderr, _T("Unknown option: %s\n"), option);
            return EXIT_FAILURE;
        }
        filesIndex = 2;
    }

    TCHAR const* const img1Path = argv[filesIndex];
    TCHAR const* const img2Path = argv[filesIndex + 1];
    TCHAR const* const mapPath  = (argc-filesIndex == 3) ? argv[filesIndex+2] : NULL;

    int width1, height1, channelCount1;
    stbi_uc* img1  = load_img(img1Path, &width1, &height1, &channelCount1);
    if (img1 == NULL)
        return EXIT_FAILURE;

    int width2, height2, channelCount2;
    stbi_uc* img2  = load_img(img2Path, &width2, &height2, &channelCount2);
    if (img2 == NULL)
    {
        stbi_image_free(img1);
        return EXIT_FAILURE;
    }

    float* map             = NULL;
    int    mapChannelCount = 0;
    if (mapPath != NULL)
    {
        mapChannelCount = (onlyChannel >= 0 || luminance) ? 1 : channelCount1;
        map = new float[width1 * height1 * mapChannelCount];
        if (map == NULL)
        {
            fprintf(stderr, "Not enough memory\n");
            stbi_image_free(img2);
            stbi_image_free(img1);
            return EXIT_FAILURE;
        }
    }

    int retval = EXIT_FAILURE;
    if (width1 != width2 || height1 != height2)
        fprintf(stderr, "Images do not have the same dimensions: %ux%u vs %ux%u\n", width1, height1, width2, height2);
    else if( channelCount1 != channelCount2)
        fprintf(stderr, "Images do not have the same number of channels: %u vs %u\n", channelCount1, channelCount2);
    else if (onlyChannel >=0 && onlyChannel >= channelCount1)
        fprintf(stderr, "Cannot compute SSIM for channel %u, images have only %u channels\n", onlyChannel, channelCount1);
    else if (convert_to_yuv && onlyChannel >=0)
        fprintf(stderr, "To compute SSIM in the yuv color space, you need a file with at least 3 channels. "
                        "Do not use the \"-#\" or \"-y\" options\n");
    else
        retval = compute_ssims(img1, img2, width1, height1, channelCount1, onlyChannel, luminance, convert_to_yuv, map, mapChannelCount);

    if (retval == EXIT_SUCCESS && mapPath != NULL)
    {
        const TCHAR* ext = _tcsrchr(mapPath, _T('.'));
        MapFormat mapFormat = MAP_FORMAT_TGA;
        if (ext == NULL)
            fprintf(stderr, "Cannot deduce file format from extension, saving as tga\n");
        else if (_tcsicmp(ext, _T(".bmp")) == 0)
            mapFormat = MAP_FORMAT_BMP;
        else if (_tcsicmp(ext, _T(".png")) == 0)
            mapFormat = MAP_FORMAT_PNG;
        else if (_tcsicmp(ext, _T(".tga")) == 0)
            mapFormat = MAP_FORMAT_TGA;
        else if (_tcsicmp(ext, _T(".pfm")) == 0)
        {
            mapFormat = MAP_FORMAT_PFM;
            if (mapChannelCount != 1 && mapChannelCount != 3)
            {
                _ftprintf(stderr, _T("PFM images can only contain 1 or 3 channels but the map contains %d channels\n"), mapChannelCount);
                retval = EXIT_FAILURE;
            }
        }
        else
            retval = EXIT_FAILURE;

        if (retval == EXIT_SUCCESS)
        {
            FILE* mapFile = _tfopen(mapPath, _T("wb"));
            if (mapFile == NULL)
            {
                _ftprintf(stderr, _T("Failed to open file \"%s\" for writing\n"), mapPath);
                retval = EXIT_FAILURE;
            }
            else
            {
                const unsigned mapSize = width1 * height1 * mapChannelCount;
                uint8_t* map8 = new uint8_t[mapSize];
                if (map8 == NULL)
                {
                    fprintf(stderr, "Not enough memory\n");
                    retval = EXIT_FAILURE;
                }
                else
                {
                    for (unsigned i=0; i<mapSize; ++i)
                        map8[i] = static_cast<uint8_t>(std::max(0.0f, map[i]) * 255.0f);

                    switch (mapFormat)
                    {
                        case MAP_FORMAT_BMP:
                            stbi_write_bmp_to_func(stbi__stdio_write, mapFile, width1, height1, mapChannelCount, map8);
                            break;
                        case MAP_FORMAT_TGA:
                            stbi_write_tga_to_func(stbi__stdio_write, mapFile, width1, height1, mapChannelCount, map8);
                            break;
                        case MAP_FORMAT_PNG:
                            stbi_write_png_to_func(stbi__stdio_write, mapFile, width1, height1, mapChannelCount, map8, width1*mapChannelCount);
                            break;
                        case MAP_FORMAT_PFM:       
                            {
#if RMGR_ARCH_IS_LITTLE_ENDIAN
                                #define PFM_ENDIANNESS  "-1.0"
#elif RMGR_ARCH_IS_BIG_ENDIAN
                                #define PFM_ENDIANNESS  "1.0"
#else
    #error Unsupported (or unknown) endianness
#endif
                                fprintf(mapFile, "P%c\n%d %d\n" PFM_ENDIANNESS "\n", (mapChannelCount==1)?'f':'F', width1, height1);
                                const size_t mapStride = width1 * mapChannelCount;
                                for (int y = height1; --y >= 0;) 
                                {
                                    if (fwrite(map+y*mapStride, sizeof(float), mapStride, mapFile) != mapStride)
                                    {
                                        _ftprintf(stderr, _T("Error writing to file \"%s\"\n"), mapPath);
                                        retval = EXIT_FAILURE;
                                        break;
                                    }
                                }
                            }
                            break;
                    }
                }

                fclose(mapFile);
            }
        }
    }

    delete[] map;
    stbi_image_free(img2);
    stbi_image_free(img1);
    return retval;
}
