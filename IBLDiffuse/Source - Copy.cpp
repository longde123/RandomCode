#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdint.h>
#include <vector>
#include <array>
#include <thread>
#include <atomic>

// stb_image is an amazing header only image library (aka no linking, just include the headers!).  http://nothings.org/stb
#pragma warning( disable : 4996 ) 
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#pragma warning( default : 4996 ) 

// for debugging. Set to 1 to make it all run on the main thread.
#define FORCE_SINGLETHREADED() 0

// Source images will be resized to this width and height in memory if they are larger than this, before convolution
#define MAX_SOURCE_IMAGE_SIZE() 64

// Destination images will be resized to this width and height in memory if they are larger than this, after convolution
#define MAX_OUTPUT_IMAGE_SIZE() 64 // TODO: temp!

// If true, converts source images to linear before doing work. Output files are linear.
#define SOURCE_IS_SRGB() 1

// ==================================================================================================================
const float c_pi = 3.14159265359f;

// ==================================================================================================================
typedef uint8_t uint8;

template <size_t N>
using TVector = std::array<float, N>;

typedef TVector<3> TVector3;
typedef TVector<2> TVector2;

// ==================================================================================================================

float sRGBToLinear(float value)
{
    if (value < 0.04045f)
        return value / 12.92f;
    else
        return std::powf(((value + 0.055f) / 1.055f), 2.4f);
}

float LinearTosRGB(float value)
{
    if (value < 0.0031308f)
        return value * 12.92f;
    else
        return std::powf(value, 1.0f / 2.4f) *  1.055f - 0.055f;
}

// ==================================================================================================================
template <size_t N>
inline TVector<N> operator + (const TVector<N>& a, const TVector<N>& b)
{
    TVector<N> result;
    for (size_t i = 0; i < N; ++i)
        result[i] = a[i] + b[i];
    return result;
}

template <size_t N>
inline TVector<N> operator * (const TVector<N>& a, const TVector<N>& b)
{
    TVector<N> result;
    for (size_t i = 0; i < N; ++i)
        result[i] = a[i] * b[i];
    return result;
}

template <size_t N>
inline TVector<N> operator + (const TVector<N>& a, float b)
{
    TVector<N> result;
    for (size_t i = 0; i < N; ++i)
        result[i] = a[i] + b;
    return result;
}

template <size_t N>
inline TVector<N> operator * (const TVector<N>& a, float b)
{
    TVector<N> result;
    for (size_t i = 0; i < N; ++i)
        result[i] = a[i] * b;
    return result;
}

template <size_t N>
inline TVector<N> operator / (const TVector<N>& a, float b)
{
    TVector<N> result;
    for (size_t i = 0; i < N; ++i)
        result[i] = a[i] / b;
    return result;
}

template <size_t N>
float LenSquared (const TVector<N>& a)
{
    float length = 0.0f;
    for (size_t i = 0; i < N; ++i)
        length += a[i] * a[i];
    return length;
}

template <size_t N>
float Len (const TVector<N>& a)
{
    return std::sqrt(LenSquared(a));
}

template <size_t N>
void Normalize (TVector<N>& a)
{
    float length = Len(a);
    for (size_t i = 0; i < N; ++i)
        a[i] /= length;
}

template <size_t N>
inline float Dot (const TVector<N>& a, const TVector<N>& b)
{
    float result = 0.0f;
    for (size_t i = 0; i < N; ++i)
        result += a[i] * b[i];
    return result;
}

template <typename T>
inline T Clamp (T value, T min, T max)
{
    if (value < min)
        return min;
    else if (value > max)
        return max;
    else
        return value;
}

// ============================================================================================
// from http://www.rorydriscoll.com/2012/01/15/cubemap-texel-solid-angle/
inline float AreaElement (float x, float y)
{
    return std::atan2(x * y, std::sqrt(x * x + y * y + 1));
}
 
// ============================================================================================
//                                     SBlockTimer
// ============================================================================================
struct SBlockTimer
{
    SBlockTimer (const char* label)
    {
        m_start = std::chrono::high_resolution_clock::now();
        m_label = label;
    }

    ~SBlockTimer ()
    {
        std::chrono::duration<float> seconds = std::chrono::high_resolution_clock::now() - m_start;
        printf("%s took %0.2f seconds\n", m_label, seconds.count());
    }

    std::chrono::high_resolution_clock::time_point m_start;
    const char* m_label;
};

// ==================================================================================================================
struct SImageData
{
    SImageData ()
        : m_width(0)
        , m_height(0)
    { }

    size_t Pitch() const { return m_width * 3; }
  
    size_t m_width;
    size_t m_height;
    std::vector<float> m_pixels;
};

// ==================================================================================================================
std::array<SImageData, 6> g_srcImages;
std::array<SImageData, 6> g_destImages;

// ==================================================================================================================
// t is a value that goes from 0 to 1 to interpolate in a C1 continuous way across uniformly sampled data points.
// when t is 0, this will return B.  When t is 1, this will return C.  Inbetween values will return an interpolation
// between B and C.  A and B are used to calculate slopes at the edges.
// More info at: https://blog.demofox.org/2015/08/15/resizing-images-with-bicubic-interpolation/
float CubicHermite (float A, float B, float C, float D, float t)
{
    float a = -A / 2.0f + (3.0f*B) / 2.0f - (3.0f*C) / 2.0f + D / 2.0f;
    float b = A - (5.0f*B) / 2.0f + 2.0f*C - D / 2.0f;
    float c = -A / 2.0f + C / 2.0f;
    float d = B;

    return a*t*t*t + b*t*t + c*t + d;
}

// ==================================================================================================================
const float* GetPixelClamped (const SImageData& image, int x, int y)
{
    x = Clamp<int>(x, 0, (int)image.m_width - 1);
    y = Clamp<int>(y, 0, (int)image.m_height - 1);
    return &image.m_pixels[(y * image.Pitch()) + x * 3];
}

// ==================================================================================================================
std::array<float, 3> SampleBicubic (const SImageData& image, float u, float v)
{
    // calculate coordinates -> also need to offset by half a pixel to keep image from shifting down and left half a pixel
    float x = (u * image.m_width) - 0.5f;
    int xint = int(x);
    float xfract = x - floor(x);

    float y = (v * image.m_height) - 0.5f;
    int yint = int(y);
    float yfract = y - floor(y);

    // 1st row
    auto p00 = GetPixelClamped(image, xint - 1, yint - 1);
    auto p10 = GetPixelClamped(image, xint + 0, yint - 1);
    auto p20 = GetPixelClamped(image, xint + 1, yint - 1);
    auto p30 = GetPixelClamped(image, xint + 2, yint - 1);

    // 2nd row
    auto p01 = GetPixelClamped(image, xint - 1, yint + 0);
    auto p11 = GetPixelClamped(image, xint + 0, yint + 0);
    auto p21 = GetPixelClamped(image, xint + 1, yint + 0);
    auto p31 = GetPixelClamped(image, xint + 2, yint + 0);

    // 3rd row
    auto p02 = GetPixelClamped(image, xint - 1, yint + 1);
    auto p12 = GetPixelClamped(image, xint + 0, yint + 1);
    auto p22 = GetPixelClamped(image, xint + 1, yint + 1);
    auto p32 = GetPixelClamped(image, xint + 2, yint + 1);

    // 4th row
    auto p03 = GetPixelClamped(image, xint - 1, yint + 2);
    auto p13 = GetPixelClamped(image, xint + 0, yint + 2);
    auto p23 = GetPixelClamped(image, xint + 1, yint + 2);
    auto p33 = GetPixelClamped(image, xint + 2, yint + 2);

    // interpolate bi-cubically!
    // Clamp the values since the curve can put the value below 0 or above 1
    std::array<float, 3> ret;
    for (int i = 0; i < 3; ++i)
    {
        float col0 = CubicHermite(p00[i], p10[i], p20[i], p30[i], xfract);
        float col1 = CubicHermite(p01[i], p11[i], p21[i], p31[i], xfract);
        float col2 = CubicHermite(p02[i], p12[i], p22[i], p32[i], xfract);
        float col3 = CubicHermite(p03[i], p13[i], p23[i], p33[i], xfract);
        float value = CubicHermite(col0, col1, col2, col3, yfract);
        ret[i] = Clamp(value, 0.0f, 1.0f);
    }
    return ret;
}

// ==================================================================================================================
void WaitForEnter ()
{
    printf("\nPress Enter to quit");
    fflush(stdin);
    getchar();
}

 // ==================================================================================================================
bool LoadImage (const char *fileName, SImageData& imageData)
{
    // load the image if we can
    int channels, width, height;
    stbi_uc* pixels = stbi_load(fileName, &width, &height, &channels, 3);
    if (!pixels)
        return false;

    // convert the pixels to float, and linear if they aren't already
    imageData.m_width = width;
    imageData.m_height = height;
    imageData.m_pixels.resize(imageData.Pitch()*imageData.m_height);

    for (size_t i = 0; i < imageData.m_pixels.size(); ++i)
    {
        imageData.m_pixels[i] = float(pixels[i]) / 255.0f;

        #if SOURCE_IS_SRGB()
        imageData.m_pixels[i] = sRGBToLinear(imageData.m_pixels[i]);
        #endif
    }

    // free the source image and return success
    stbi_image_free(pixels);
    return true;
}

// ==================================================================================================================
bool SaveImage(const char *fileName, const SImageData &image)
{
    std::vector<uint8> outPixels;
    outPixels.resize(image.m_pixels.size());
    for (size_t i = 0; i < image.m_pixels.size(); ++i)
    {
        float value = image.m_pixels[i];
        outPixels[i] = uint8(value * 255.0f);
    }

    return stbi_write_png(fileName, (int)image.m_width, (int)image.m_height, 3, &outPixels[0], (int)image.Pitch()) == 1;
}

// ==================================================================================================================
void GetFaceBasis (size_t faceIndex, TVector3& facePlane, TVector3& uAxis, TVector3& vAxis)
{
    facePlane = { 0, 0, 0 };
    facePlane[faceIndex % 3] = (faceIndex / 3) ? 1.0f : -1.0f;
    uAxis = { 0, 0, 0 };
    vAxis = { 0, 0, 0 };
    switch (faceIndex % 3)
    {
        case 0:
        {
            uAxis[2] = (faceIndex / 3) ? 1.0f : -1.0f;
            vAxis[1] = 1.0f;
            break;
        }
        case 1:
        {
            uAxis[0] = 1.0f;
            vAxis[2] = (faceIndex / 3) ? 1.0f : -1.0f;
            break;
        }
        case 2:
        {
            uAxis[0] = (faceIndex / 3) ? 1.0f : -1.0f;
            vAxis[1] = 1.0f;
        }
    }

    if ((faceIndex % 3) == 2)
    {
        facePlane[2] *= -1.0f;
    }

    /*
    "%sLeft.bmp",
    "%sDown.bmp",
    "%sBack.bmp",
    "%sRight.bmp",
    "%sUp.bmp",
    "%sFront.bmp",

    */
}

// ==================================================================================================================
TVector3 DiffuseIrradianceForNormal (const TVector3& normal)
{
    // loop through every pixel in the source cube map and add that pixel's contribution to the diffuse irradiance
    float totalWeight = 0.0f;
    TVector3 irradiance = { 0.0f, 0.0f, 0.0f };
    for (size_t faceIndex = 0; faceIndex < 6; ++faceIndex)
    {
        const SImageData& src = g_srcImages[faceIndex];
        TVector3 facePlane, uAxis, vAxis;
        GetFaceBasis(faceIndex, facePlane, uAxis, vAxis);

        float invResolution = 1.0f / float(src.m_width);

        for (size_t iy = 0, iyc = src.m_height; iy < iyc; ++iy)
        {
            TVector2 uv;
            uv[1] = ((float(iy) + 0.5f) / float(iyc));

            for (size_t ix = 0, ixc = src.m_width; ix < ixc; ++ix)
            {
                const float* pixel = &src.m_pixels[iy * src.Pitch() + ix * 3];

                uv[0] = ((float(ix) + 0.5f) / float(ixc));

                // only accept directions where dot product greater than 0
                TVector3 sampleDir =
                    facePlane +
                    uAxis * (uv[0] * 2.0f - 1.0f) +
                    vAxis * (uv[1] * 2.0f - 1.0f);
                Normalize(sampleDir);

                float cosTheta = Dot(normal, sampleDir);
                if (cosTheta <= 0.0f)
                {
                    continue;
                }

                // get the pixel color
                TVector3 pixelColor =
                {
                    pixel[0],
                    pixel[1],
                    pixel[2],
                };

                // calculate solid angle (size) of the pixel
                float x0 = uv[0] - invResolution;
                float y0 = uv[1] - invResolution;
                float x1 = uv[0] + invResolution;
                float y1 = uv[1] + invResolution;
                float solidAngle = AreaElement(x0, y0) - AreaElement(x0, y1) - AreaElement(x1, y0) + AreaElement(x1, y1);

                // add this pixel's contribution into the radiance
                irradiance = irradiance + pixelColor *solidAngle * cosTheta;

                // keep track of the total weight so we can normalize later
                totalWeight += solidAngle;
            }
        }
    }
    
    irradiance = irradiance * 4.0f / totalWeight;

    return irradiance;
}

// ==================================================================================================================
void OnRowComplete (size_t rowIndex, size_t numRows)
{
    // report progress
    int oldPercent = rowIndex > 0 ? (int)(100.0f * float(rowIndex - 1) / float(numRows)) : 0;
    int newPercent = (int)(100.0f * float(rowIndex) / float(numRows));
    if (oldPercent != newPercent)
        printf("\r               \rProgress: %i%%", newPercent);
}

// ==================================================================================================================
void ProcessRow (size_t rowIndex)
{
    size_t faceIndex = 0;
    while (rowIndex >= g_srcImages[faceIndex].m_height)
    {
        rowIndex -= g_srcImages[faceIndex].m_height;
        ++faceIndex;
    }

    TVector3 facePlane, uAxis, vAxis;
    GetFaceBasis(faceIndex, facePlane, uAxis, vAxis);

    SImageData &destData = g_destImages[faceIndex];
    TVector2 uv;
    uv[1] = ((float(rowIndex) + 0.5f) / float(destData.m_height));
    for (size_t ix = 0; ix < destData.m_width; ++ix)
    {
        uv[0] = ((float(ix) + 0.5f) / float(destData.m_width));

        // calculate the position of the pixel on the cube
        TVector3 normalDir =
            facePlane +
            uAxis * (uv[0] * 2.0f - 1.0f) +
            vAxis * (uv[1] * 2.0f - 1.0f);
        Normalize(normalDir);

        // get the diffuse irradiance for this direction
        TVector3 diffuseIrradiance = DiffuseIrradianceForNormal(normalDir);

        // store the resulting color
        float* pixel = &destData.m_pixels[rowIndex * destData.Pitch() + ix * 3];
        pixel[0] = Clamp(diffuseIrradiance[0], 0.0f, 1.0f);
        pixel[1] = Clamp(diffuseIrradiance[1], 0.0f, 1.0f);
        pixel[2] = Clamp(diffuseIrradiance[2], 0.0f, 1.0f);
    }
}

// ==================================================================================================================
void ConvolutionThreadFunc ()
{
    static std::atomic<size_t> s_rowIndex(0);

    size_t numRows = 0;
    for (const SImageData& image : g_srcImages)
        numRows += image.m_height;

    size_t rowIndex = s_rowIndex.fetch_add(1);
    while (rowIndex < numRows)
    {
        ProcessRow(rowIndex);
        OnRowComplete(rowIndex, numRows);
        rowIndex = s_rowIndex.fetch_add(1);
    }
}

// ==================================================================================================================
void DownsizeImage (SImageData& image, size_t imageSize)
{
    // repeatedly cut image in half (at max) until we reach the desired size.
    // done this way to avoid aliasing.
    while (image.m_width > imageSize)
    {
        // calculate new image size
        size_t newImageSize = image.m_width / 2;
        if (newImageSize < imageSize)
            newImageSize = imageSize;

        // allocate new image
        SImageData newImage;
        newImage.m_width = newImageSize;
        newImage.m_height = newImageSize;
        newImage.m_pixels.resize(newImage.m_height * newImage.Pitch());

        // sample pixels
        for (size_t iy = 0, iyc = newImage.m_height; iy < iyc; ++iy)
        {
            float percentY = float(iy) / float(iyc);

            float* destPixel = &newImage.m_pixels[iy * newImage.Pitch()];
            for (size_t ix = 0, ixc = newImage.m_width; ix < ixc; ++ix)
            {
                float percentX = float(ix) / float(ixc);

                std::array<float, 3> srcSample = SampleBicubic(image, percentX, percentY);
                destPixel[0] = srcSample[0];
                destPixel[1] = srcSample[1];
                destPixel[2] = srcSample[2];

                destPixel += 3;
            }
        }

        // set the image to the new image to possibly go through the loop again
        image = newImage;
    }
}

// ==================================================================================================================
void DownsizeSourceThreadFunc ()
{
    static std::atomic<size_t> s_imageIndex(0);
    size_t imageIndex = s_imageIndex.fetch_add(1);
    while (imageIndex < 6)
    {
        // downsize
        DownsizeImage(g_srcImages[imageIndex], MAX_SOURCE_IMAGE_SIZE());

        // initialize destination image
        g_destImages[imageIndex].m_width = g_srcImages[imageIndex].m_width;
        g_destImages[imageIndex].m_height = g_srcImages[imageIndex].m_height;
        g_destImages[imageIndex].m_pixels.resize(g_destImages[imageIndex].m_height * g_destImages[imageIndex].Pitch());

        // get next image to process
        imageIndex = s_imageIndex.fetch_add(1);
    }
}

// ==================================================================================================================
void DownsizeOutputThreadFunc ()
{
    static std::atomic<size_t> s_imageIndex(0);
    size_t imageIndex = s_imageIndex.fetch_add(1);
    while (imageIndex < 6)
    {
        // downsize
        DownsizeImage(g_destImages[imageIndex], MAX_OUTPUT_IMAGE_SIZE());

        // get next image to process
        imageIndex = s_imageIndex.fetch_add(1);
    }
}

// ==================================================================================================================
template <typename L>
void RunMultiThreaded (const char* label, const L& lambda, bool newline)
{
    SBlockTimer timer(label);
    size_t numThreads = FORCE_SINGLETHREADED() ? 1 : std::thread::hardware_concurrency();
    printf("Doing %s with %zu threads.\n", label, numThreads);
    if (numThreads > 1)
    {
        std::vector<std::thread> threads;
        threads.resize(numThreads);
        size_t faceIndex = 0;
        for (std::thread& t : threads)
            t = std::thread(lambda);
        for (std::thread& t : threads)
            t.join();
    }
    else
    {
        lambda();
    }
    if (newline)
        printf("\n");
}

// ==================================================================================================================
int main (int argc, char **argv)
{
    //const char* src = "Vasa\\Vasa";
    const char* src = "ame_ash\\ashcanyon";
    //const char* src = "DallasW\\dallas";
    //const char* src = "MarriottMadisonWest\\Marriot";
    //const char* src = "mnight\\mnight";

    const char* srcPatterns[6] = {
        "%sLeft.bmp",
        "%sDown.bmp",
        "%sBack.bmp",
        "%sRight.bmp",
        "%sUp.bmp",
        "%sFront.bmp",
    };

    const char* destPatterns[6] = {
        "%sDiffuseLeft.png",
        "%sDiffuseDown.png",
        "%sDiffuseBack.png",
        "%sDiffuseRight.png",
        "%sDiffuseUp.png",
        "%sDiffuseFront.png",
    };

    // try and load the source images, while initializing the destination images
    for (size_t i = 0; i < 6; ++i)
    {
        // load source image if we can
        char srcFileName[256];
        sprintf(srcFileName, srcPatterns[i], src);
        if (LoadImage(srcFileName, g_srcImages[i]))
        {
            printf("Loaded: %s (%zu x %zu)\n", srcFileName, g_srcImages[i].m_width, g_srcImages[i].m_height);

            if (g_srcImages[i].m_width != g_srcImages[i].m_height)
            {
                printf("image is not square!\n");
                WaitForEnter();
                return 0;
            }
        }
        else
        {
            printf("Could not load image: %s\n", srcFileName);
            WaitForEnter();
            return 0;
        }
    }

    // verify that the images are all the same size
    for (size_t i = 1; i < 6; ++i)
    {
        if (g_srcImages[i].m_width != g_srcImages[0].m_width || g_srcImages[i].m_height != g_srcImages[0].m_height)
        {
            printf("images are not all the same size!\n");
            WaitForEnter();
            return 0;
        }
    }

    // Resize source images in memory
    if (g_srcImages[0].m_width > MAX_SOURCE_IMAGE_SIZE())
    {
        printf("\nDownsizing source images in memory to %i x %i\n", MAX_SOURCE_IMAGE_SIZE(), MAX_SOURCE_IMAGE_SIZE());
        RunMultiThreaded("Downsize source image", DownsizeSourceThreadFunc, false);
    }

    // Do the convolution
    printf("\n");
    RunMultiThreaded("convolution", ConvolutionThreadFunc, true);

    // Resize destination images in memory
    if (g_srcImages[0].m_width > MAX_OUTPUT_IMAGE_SIZE())
    {
        printf("\nDownsizing output images in memory to %i x %i\n", MAX_OUTPUT_IMAGE_SIZE(), MAX_OUTPUT_IMAGE_SIZE());
        RunMultiThreaded("Downsize output image", DownsizeOutputThreadFunc, false);
    }

    // save the resulting images
    printf("\n");
    for (size_t i = 0; i < 6; ++i)
    {
        char destFileName[256];
        sprintf(destFileName, destPatterns[i], src);
        if (SaveImage(destFileName, g_destImages[i]))
        {
            printf("Saved: %s\n", destFileName);
        }
        else
        {
            printf("Could not save image: %s\n", destFileName);
            WaitForEnter();
            return 0;
        }
    }

    WaitForEnter();
    return 0;
}

/*

TODO:
? what size is typical of "shrinking to" before convolution (or, source capture size)?
* What size should the destination images be? The tutorial says 32x32 works.
* use this in the webgl program before making blog post!  That will help verify correctness, but also will give you screenshots.

Blog:
* note problems with UV's from different sources! needed to adjust images to be correct
* Link to PBR / IBL tutorial: https://learnopengl.com/#!PBR/IBL/Diffuse-irradiance
 * show how it fits into the equations
* mention the thing about needing an HDR image format in reality.
* skyboxes from: http://www.custommapmakers.org/skyboxes.php
* and: https://opengameart.org/content/indoors-skyboxes
* There are for sure faster ways to do this, but writing for clarity.
 * for instance, could just have a for each pixel in every destination image... for each pixel in every source image...
 * I'd bet that is faster than calling SSampleSourceCubeMap so many times for each pixel just to find the pixel location.
* talk about how you need 24 bit bmps? mspaint seems to save them ok. gimp made 32 bit bmps even though only RGB. wasted A channel? i dunno. maybe use different loading code if using this for real :P
* talk about how it's only low frequency content (smooth), so is a good candidate for aproximating with functions - like spherical harmonics.
 * Do a DFT of image to show this? Not sure if it will apply because DFT assumes cyclical data, which would make high frequency content at the edges.  see what kind of results DFT gives.
 * Would need to do a spherical DFT - which is what spherical harmonics are!
 * future post
* solid angle of a texel: http://www.rorydriscoll.com/2012/01/15/cubemap-texel-solid-angle/
* don't need HDR format out unless you take HDR input in.  The output pixels are a weighted average of input pixels where the weights are less than 1.  The highest valued output pixel will be <= the highest valued input pixel.
* usually done on GPU and runs much faster there (does it usually?)
* make and provide zip of bmp files.
* feels like a waste having it spend all that time to calculate this thing that has such low detail hehe.
* note sRGB correction may need to happen on load and writing back out, otherwise integration will be incorrect.

https://www.gamedev.net/topic/675390-diffuse-ibl-importance-sampling-vs-spherical-harmonics/

http://blog.selfshadow.com/publications/s2014-shading-course/frostbite/s2014_pbs_frostbite_slides.pdf

https://seblagarde.wordpress.com/2012/06/10/amd-cubemapgen-for-physically-based-rendering/

Calculating solid angle of a texel in a cube map
http://www.rorydriscoll.com/2012/01/15/cubemap-texel-solid-angle/

*/