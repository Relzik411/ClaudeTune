#pragma once
// ClaudeTune - minimal in-place radix-2 complex FFT.
// One twiddle table is built for the largest size; smaller power-of-two sizes
// reuse it with a stride, so changing size never allocates on the audio thread.

#include <complex>
#include <vector>
#include <cmath>
#include <utility>

namespace claudetune
{

class FFT
{
public:
    void prepare (int maxSize)
    {
        maxN = 1;
        while (maxN < maxSize)
            maxN <<= 1;

        twiddles.resize ((size_t) maxN / 2);
        const double twoPi = 6.283185307179586476925286766559;
        for (int k = 0; k < maxN / 2; ++k)
            twiddles[(size_t) k] = std::polar (1.0f, (float) (-twoPi * k / maxN));
    }

    int getMaxSize() const noexcept { return maxN; }

    // n must be a power of two <= getMaxSize(). inverse == true applies 1/n scaling.
    void transform (std::complex<float>* d, int n, bool inverse) const noexcept
    {
        // bit-reversal permutation
        for (int i = 1, j = 0; i < n; ++i)
        {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1)
                j ^= bit;
            j ^= bit;
            if (i < j)
                std::swap (d[i], d[j]);
        }

        for (int len = 2; len <= n; len <<= 1)
        {
            const int half = len >> 1;
            const int stride = maxN / len;
            for (int i = 0; i < n; i += len)
            {
                for (int j = 0; j < half; ++j)
                {
                    auto w = twiddles[(size_t) (j * stride)];
                    if (inverse)
                        w = std::conj (w);
                    const auto u = d[i + j];
                    const auto v = d[i + j + half] * w;
                    d[i + j] = u + v;
                    d[i + j + half] = u - v;
                }
            }
        }

        if (inverse)
        {
            const float scale = 1.0f / (float) n;
            for (int i = 0; i < n; ++i)
                d[i] *= scale;
        }
    }

private:
    int maxN = 0;
    std::vector<std::complex<float>> twiddles;
};

} // namespace claudetune
