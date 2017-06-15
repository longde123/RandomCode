#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdint.h>
#include <vector>
#include <array>
#include <windows.h>  // for bitmap headers.  Sorry non windows people!
#include <thread>
#include <atomic>

// TODO: make sure this is 0 when you are done
#define FORCE_SINGLETHREADED() 1

// ==================================================================================================================
const float c_pi = 3.14159265359f;

// ==================================================================================================================
typedef uint8_t uint8;

template <size_t N>
using TVector = std::array<float, N>;

typedef TVector<3> TVector3;
typedef TVector<2> TVector2;

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
inline size_t LargestMagnitudeComponent (const TVector<N>& a)
{
    size_t winningIndex = 0;
    for (size_t i = 1; i < N; ++i)
    {
        if (std::abs(a[i]) > std::abs(a[winningIndex]))
            winningIndex = i;
    }
    return winningIndex;
}

inline TVector<3> Cross (const TVector<3>& a, const TVector<3>& b)
{
    return
    {
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0]
    };
}

// ============================================================================================
// these two functions are taken from http://www.rorydriscoll.com/2012/01/15/cubemap-texel-solid-angle/
inline float AreaElement (float x, float y)
{
    return std::atan2(x * y, std::sqrt(x * x + y * y + 1));
}
 
// ============================================================================================
float TexelCoordSolidAngle (size_t faceIndex, TVector2 uv, size_t imageWidthHeight)
{
    //scale up to [-1, 1] range (inclusive), offset by 0.5 to point to texel center.
    //float U = (2.0f * ((float)uv[0] + 0.5f) / (float)imageWidthHeight) - 1.0f;
    //float V = (2.0f * ((float)uv[1] + 0.5f) / (float)imageWidthHeight) - 1.0f;
    
    // TODO: can keep track of uv's in -1,1 space at center of pixel (+ 0.5) in loop.  have a current and next. At end of loop, current = next, and recalculate next.
    // TODO: can keep track of x0,x1,y0,y1 in loops with current / next like above. x's in x loop. y's in y loop.
    // TODO: can also keep track of AreaElement current and next like above, inside of x loop.
    // TODO: after that, all we have is shrinking the starting image size

    float U = (uv[0] + 0.5f / float(imageWidthHeight)) * 2.0f - 1.0f;
    float V = (uv[0] + 0.5f / float(imageWidthHeight)) * 2.0f - 1.0f;
 
    float InvResolution = 1.0f / float(imageWidthHeight);
 
    // U and V are the -1..1 texture coordinate on the current face.
    // Get projected area for this texel
    float x0 = U - InvResolution;
    float y0 = V - InvResolution;
    float x1 = U + InvResolution;
    float y1 = V + InvResolution;
    float SolidAngle = AreaElement(x0, y0) - AreaElement(x0, y1) - AreaElement(x1, y0) + AreaElement(x1, y1);
 
    return SolidAngle;
}

// ============================================================================================
//                                     SBlockTimer
// ============================================================================================
struct SBlockTimer
{
    SBlockTimer(const char* label)
    {
        m_start = std::chrono::high_resolution_clock::now();
        m_label = label;
    }

    ~SBlockTimer()
    {
        std::chrono::duration<float> seconds = std::chrono::high_resolution_clock::now() - m_start;
        printf("%s%0.2f seconds\n", m_label, seconds.count());
    }

    std::chrono::high_resolution_clock::time_point m_start;
    const char* m_label;
};

// ==================================================================================================================
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

// ==================================================================================================================
 struct SImageData
{
    SImageData()
        : m_width(0)
        , m_height(0)
    { }
  
    size_t m_width;
    size_t m_height;
    size_t m_pitch;
    std::vector<uint8> m_pixels;
};

// ==================================================================================================================
std::array<SImageData, 6> g_srcImages;
std::array<SImageData, 6> g_destImages;

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
    // open the file if we can
    FILE *file;
    file = fopen(fileName, "rb");
    if (!file)
        return false;
  
    // read the headers if we can
    BITMAPFILEHEADER header;
    BITMAPINFOHEADER infoHeader;
    if (fread(&header, sizeof(header), 1, file) != 1 ||
        fread(&infoHeader, sizeof(infoHeader), 1, file) != 1 ||
        header.bfType != 0x4D42 || infoHeader.biBitCount != 24)
    {
        fclose(file);
        return false;
    }
  
    // read in our pixel data if we can. Note that it's in BGR order, and width is padded to the next power of 4
    imageData.m_pixels.resize(infoHeader.biSizeImage);
    fseek(file, header.bfOffBits, SEEK_SET);
    if (fread(&imageData.m_pixels[0], imageData.m_pixels.size(), 1, file) != 1)
    {
        fclose(file);
        return false;
    }
  
    imageData.m_width = infoHeader.biWidth;
    imageData.m_height = infoHeader.biHeight;
    imageData.m_pitch = 4 * ((imageData.m_width * 24 + 31) / 32);
  
    fclose(file);
    return true;
}

// ==================================================================================================================
bool SaveImage (const char *fileName, const SImageData &image)
{
    // open the file if we can
    FILE *file;
    file = fopen(fileName, "wb");
    if (!file)
        return false;
  
    // make the header info
    BITMAPFILEHEADER header;
    BITMAPINFOHEADER infoHeader;
  
    header.bfType = 0x4D42;
    header.bfReserved1 = 0;
    header.bfReserved2 = 0;
    header.bfOffBits = 54;
  
    infoHeader.biSize = 40;
    infoHeader.biWidth = (long)image.m_width;
    infoHeader.biHeight = (long)image.m_height;
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 24;
    infoHeader.biCompression = 0;
    infoHeader.biSizeImage = (DWORD)image.m_pixels.size();
    infoHeader.biXPelsPerMeter = 0;
    infoHeader.biYPelsPerMeter = 0;
    infoHeader.biClrUsed = 0;
    infoHeader.biClrImportant = 0;
  
    header.bfSize = infoHeader.biSizeImage + header.bfOffBits;
  
    // write the data and close the file
    fwrite(&header, sizeof(header), 1, file);
    fwrite(&infoHeader, sizeof(infoHeader), 1, file);
    fwrite(&image.m_pixels[0], infoHeader.biSizeImage, 1, file);
    fclose(file);
    return true;
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
}

// ==================================================================================================================
TVector3 SampleSourceCubeMap (const TVector3& direction)
{
    size_t largestComponent = LargestMagnitudeComponent(direction);
    bool negFace = direction[largestComponent] < 0.0f;
    TVector3 cubePos = direction / direction[largestComponent];
    if (negFace)
        cubePos = cubePos * -1.0f;

    size_t faceIndex = largestComponent;
    if (!negFace)
        faceIndex += 3;

    // TODO: can we leverage GetFaceBasis() somehow? maybe dot product to get these or something?
    TVector2 uv = { 0.0f, 0.0f };
    switch (largestComponent)
    {
        case 0:
        {
            uv[0] = cubePos[2] * (negFace ? -1.0f : 1.0f);
            uv[1] = cubePos[1];
            break;
        }
        case 1:
        {
            uv[0] = cubePos[0];
            uv[1] = cubePos[2] * (negFace ? -1.0f : 1.0f);
            break;
        }
        case 2:
        {
            uv[0] = cubePos[0] * (negFace ? -1.0f : 1.0f);
            uv[1] = cubePos[1];
            break;
        }
    }
    uv = uv * 0.5f + 0.5f;

    size_t pixelX = (size_t)(float(g_srcImages[faceIndex].m_width-1) * uv[0]);
    pixelX = Clamp<size_t>(pixelX, 0, g_srcImages[faceIndex].m_width - 1);

    size_t pixelY = (size_t)(float(g_srcImages[faceIndex].m_height-1) * uv[1]);
    pixelY = Clamp<size_t>(pixelY, 0, g_srcImages[faceIndex].m_height - 1);

    size_t pixelOffset = pixelY * g_srcImages[faceIndex].m_pitch + pixelX * 3;

    return
    {
        float(g_srcImages[faceIndex].m_pixels[pixelOffset + 0]) / 255.0f,
        float(g_srcImages[faceIndex].m_pixels[pixelOffset + 1]) / 255.0f,
        float(g_srcImages[faceIndex].m_pixels[pixelOffset + 2]) / 255.0f,
    };

}

// ==================================================================================================================
TVector3 DiffuseIrradianceForNormalOld (const TVector3& normal)
{
    // adapted from https://learnopengl.com/#!PBR/IBL/Diffuse-irradiance
    TVector3 irradiance = { 0.0f, 0.0f, 0.0f };

    TVector3 up = { 0.0, 1.0, 0.0 };
    TVector3 right = Cross(up, normal);
    up = Cross(normal, right);

    float sampleDelta = 0.025f;
    size_t sampleCount = 0;
    for (float phi = 0.0f; phi < 2.0f * c_pi; phi += sampleDelta)
    {
        for (float theta = 0.0f; theta < 0.5f * c_pi; theta += sampleDelta)
        {
            // spherical to cartesian (in tangent space)
            TVector3 tangentSample = { sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta) };
            // tangent space to world
            TVector3 sampleVec = right * tangentSample[0] + up * tangentSample[1] + normal * tangentSample[2];

            irradiance = irradiance + SampleSourceCubeMap(sampleVec) * cos(theta) * sin(theta);
            ++sampleCount;
        }
    }
    irradiance = irradiance * c_pi * (1.0f / float(sampleCount));

    return irradiance;
}

// ==================================================================================================================
TVector3 DiffuseIrradianceForNormalNew (const TVector3& normal)
{
    // TODO: there is a face (at minimum? maybe more) which is entirely skipable, detect that and skip it!
    // TODO: is there any way to factor TexelCoordSolidAngle() out of the loop, at least in part?
    TVector3 irradiance = { 0.0f, 0.0f, 0.0f };
    for (size_t faceIndex = 0; faceIndex < 6; ++faceIndex)
    {
        const SImageData& src = g_srcImages[faceIndex];
        TVector3 facePlane, uAxis, vAxis;
        GetFaceBasis(faceIndex, facePlane, uAxis, vAxis);

        for (size_t iy = 0, iyc = src.m_height; iy < iyc; ++iy)
        {
            TVector2 uv = { 0.0f, float(iy) / float(iyc - 1) };

            const uint8* pixel = &src.m_pixels[iy * src.m_pitch];
            for (size_t ix = 0, ixc = src.m_width; ix < ixc; ++ix)
            {
                uv[0] = float(ix) / float(ixc - 1);
                float solidAngle = TexelCoordSolidAngle(faceIndex, uv, ixc);

                TVector3 pixelColor =
                {
                    float(pixel[0]) / 255.0f,
                    float(pixel[1]) / 255.0f,
                    float(pixel[2]) / 255.0f,
                };

                // TODO: also need cosine theta!

                irradiance = irradiance + pixelColor * solidAngle;
                pixel += 3;
            }
        }
    }
    return irradiance;
}

// ==================================================================================================================
void OnRowComplete (size_t rowIndex, size_t numrows)
{
    // report progress
    int oldPercent = rowIndex > 0 ? (int)(100.0f * float(rowIndex - 1) / float(numrows)) : 0;
    int newPercent = (int)(100.0f * float(rowIndex) / float(numrows));
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
    uint8* pixel = &destData.m_pixels[rowIndex * destData.m_pitch];
    TVector2 uv;
    uv[1] = (float(rowIndex) / float(destData.m_height - 1));
    for (size_t ix = 0; ix < destData.m_width; ++ix)
    {
        uv[0] = (float(ix) / float(destData.m_width - 1));

        // calculate the position of the pixel on the cube
        TVector3 normalDir =
            facePlane +
            uAxis * (uv[0] * 2.0f - 1.0f) +
            vAxis * (uv[1] * 2.0f - 1.0f);

        // get the diffuse irradiance for this direction
        //TVector3 diffuseIrradiance = DiffuseIrradianceForNormalOld(normalDir);
        TVector3 diffuseIrradiance = DiffuseIrradianceForNormalNew(normalDir);
        // TODO: remove comment after it's working and timed

        // store the resulting color
        pixel[0] = (uint8)Clamp(diffuseIrradiance[0] * 255.0f, 0.0f, 255.0f);
        pixel[1] = (uint8)Clamp(diffuseIrradiance[1] * 255.0f, 0.0f, 255.0f);
        pixel[2] = (uint8)Clamp(diffuseIrradiance[2] * 255.0f, 0.0f, 255.0f);
        pixel += 3;
    }
}

// ==================================================================================================================
void ThreadFunc ()
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
int main (int argc, char **argv)
{
    //const char* src = "Vasa\\Vasa";
    const char* src = "ame_ash\\ashcanyon";

    const char* srcPatterns[6] = {
        "%sLeft.bmp",
        "%sDown.bmp",
        "%sBack.bmp",
        "%sRight.bmp",
        "%sUp.bmp",
        "%sFront.bmp",
    };

    const char* destPatterns[6] = {
        "%sLeft_Diffuse.bmp",
        "%sDown_Diffuse.bmp",
        "%sBack_Diffuse.bmp",
        "%sRight_Diffuse.bmp",
        "%sUp_Diffuse.bmp",
        "%sFront_Diffuse.bmp",
    };

    // try and load the source images, while initializing the destination images
    for (size_t i = 0; i < 6; ++i)
    {
        // load source image if we can
        char srcFileName[256];
        sprintf(srcFileName, srcPatterns[i], src);
        if (LoadImage(srcFileName, g_srcImages[i]))
        {
            printf("Loaded: %s\n", srcFileName);
        }
        else
        {
            printf("Could not load image: %s\n", srcFileName);
            WaitForEnter();
            return 0;
        }

        // initialize destination image
        g_destImages[i].m_width = g_srcImages[i].m_width;
        g_destImages[i].m_height = g_srcImages[i].m_height;
        g_destImages[i].m_pitch = g_srcImages[i].m_pitch;
        g_destImages[i].m_pixels.resize(g_destImages[i].m_height * g_destImages[i].m_pitch);
    }

    // process each destination image, multithreadedly if we can / should.
    {
        SBlockTimer timer("\nConvolution took ");
        size_t numThreads = FORCE_SINGLETHREADED() ? 1 : std::thread::hardware_concurrency();
        printf("\nDoing convolution with %zu threads.\n", numThreads);
        if (numThreads > 1)
        {
            std::vector<std::thread> threads;
            threads.resize(numThreads);
            size_t faceIndex = 0;
            for (std::thread& t : threads)
                t = std::thread(ThreadFunc);
            for (std::thread& t : threads)
                t.join();
        }
        else
        {
            ThreadFunc();
        }
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
* TexelCoordSolidAngle() doesn't use face index, is that right?
* should i shrink the source images before convolution?
* Maybe switching to ForEveryPixel^2 thing. I think it will be less computation over all
? get rid of DiffuseIrradianceForNormalOld() after you time it and see what kind of difference in speed it has
* process all the skybox images you have.
* i guess the resulting image can be small.  The tutorial says 32x32?
* test 32 and 64 bit mode
* take source images from command line?
* make sure code is cleaned up etc

Blog:
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

https://www.gamedev.net/topic/675390-diffuse-ibl-importance-sampling-vs-spherical-harmonics/

http://blog.selfshadow.com/publications/s2014-shading-course/frostbite/s2014_pbs_frostbite_slides.pdf

https://seblagarde.wordpress.com/2012/06/10/amd-cubemapgen-for-physically-based-rendering/

Calculating solid angle of a texel in a cube map
http://www.rorydriscoll.com/2012/01/15/cubemap-texel-solid-angle/

*/