#pragma once
// ClaudeTune - YIN pitch detector (de Cheveigne & Kawahara 2002) with an
// FFT-computed autocorrelation term so it stays cheap at low pitches.

#include "FFT.h"
#include <vector>
#include <complex>
#include <cmath>
#include <algorithm>

namespace claudetune
{

class PitchDetector
{
public:
    // maxLength = largest (window + maxLag) that will ever be analysed
    void prepare (int maxLength)
    {
        fft.prepare (maxLength);
        const int n = fft.getMaxSize();
        z.assign ((size_t) n, {});
        prod.assign ((size_t) n, {});
        cmndf.assign ((size_t) maxLength + 1, 1.0f);
        squares.assign ((size_t) maxLength + 1, 0.0);
    }

    // x points to (window + tauMax) contiguous samples, oldest first.
    // Returns the fractional period in samples; aperiodicity receives the
    // normalised-difference value at that lag (0 = perfectly periodic).
    float detect (const float* x, int window, int tauMin, int tauMax,
                  float threshold, float& aperiodicity) noexcept
    {
        const int length = window + tauMax;
        int n = 1;
        while (n < length)
            n <<= 1;

        // Pack a = x[0..window) (real part) and b = x[0..length) (imag part)
        for (int i = 0; i < n; ++i)
        {
            const float a = i < window ? x[i] : 0.0f;
            const float b = i < length ? x[i] : 0.0f;
            z[(size_t) i] = { a, b };
        }

        fft.transform (z.data(), n, false);

        // Separate the two real spectra and form conj(A) * B
        for (int k = 0; k < n; ++k)
        {
            const auto zk = z[(size_t) k];
            const auto zn = std::conj (z[(size_t) ((n - k) & (n - 1))]);
            const auto A = (zk + zn) * 0.5f;
            const auto B = (zk - zn) * std::complex<float> (0.0f, -0.5f);
            prod[(size_t) k] = std::conj (A) * B;
        }

        fft.transform (prod.data(), n, true); // real part = r(tau)

        squares[0] = 0.0;
        for (int i = 0; i < length; ++i)
            squares[(size_t) i + 1] = squares[(size_t) i] + (double) x[i] * (double) x[i];

        const double e0 = squares[(size_t) window];
        double running = 0.0;
        cmndf[0] = 1.0f;
        for (int tau = 1; tau <= tauMax; ++tau)
        {
            const double et = squares[(size_t) (tau + window)] - squares[(size_t) tau];
            double d = e0 + et - 2.0 * (double) prod[(size_t) tau].real();
            if (d < 0.0)
                d = 0.0;
            running += d;
            cmndf[(size_t) tau] = running > 0.0 ? (float) (d * tau / running) : 1.0f;
        }

        tauMin = std::max (tauMin, 2);
        int best = -1;
        for (int tau = tauMin; tau < tauMax; ++tau)
        {
            if (cmndf[(size_t) tau] < threshold)
            {
                while (tau + 1 < tauMax && cmndf[(size_t) tau + 1] < cmndf[(size_t) tau])
                    ++tau;
                best = tau;
                break;
            }
        }

        if (best < 0)
        {
            float minVal = 1e9f;
            for (int tau = tauMin; tau < tauMax; ++tau)
                if (cmndf[(size_t) tau] < minVal)
                {
                    minVal = cmndf[(size_t) tau];
                    best = tau;
                }
        }

        if (best < 0)
        {
            aperiodicity = 1.0f;
            return 0.0f;
        }

        aperiodicity = cmndf[(size_t) best];

        float period = (float) best;
        if (best > tauMin && best < tauMax - 1)
        {
            const float a = cmndf[(size_t) best - 1];
            const float b = cmndf[(size_t) best];
            const float c = cmndf[(size_t) best + 1];
            const float den = a - 2.0f * b + c;
            if (std::abs (den) > 1e-9f)
                period += std::clamp (0.5f * (a - c) / den, -1.0f, 1.0f);
        }
        return period;
    }

private:
    FFT fft;
    std::vector<std::complex<float>> z, prod;
    std::vector<float> cmndf;
    std::vector<double> squares;
};

} // namespace claudetune
