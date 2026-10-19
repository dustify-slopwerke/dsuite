#include "PianoKey.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace dpiano
{
namespace
{
constexpr double twoPi = 2.0 * std::numbers::pi;
constexpr double t60ToSigma = 6.907755; // ln (1000): decay rate for a 60 dB fall in 1 s

// Bridge force -> output units: full scale is reached by a fortissimo chord in the middle
constexpr double outputScale = 0.0285;

// Extra decay rate (1/s) at the peak of the board's drain, at "Board Drain" 1
constexpr double boardDrainSigma = 2.0;

// Below this energy (output units squared) a key is silent and stops being processed
constexpr float silentEnergy = 5.0e-11f;
} // namespace

void PianoKey::reset()
{
    re.fill (0.0f);
    im.fill (0.0f);
    symRe.fill (0.0f);
    symIm.fill (0.0f);
    ownOutput.fill (0.0f);
    sympatheticActive = false;
    active = false;
    hammerActive = false;
    quietBlocks = 0;
}

void PianoKey::configure (int key, const Settings& s)
{
    keyData_ = keyData (key);
    const auto& d = keyData_;
    sampleRate = s.sampleRate;

    const double b = d.inharmonicity * s.inharmonicityScale;
    const double f0 = s.firstPartialHz / std::sqrt (1.0 + b);

    // String geometry from tension, mass and pitch: f0 = sqrt (T / mu) / 2L with mu = mass / L
    const double length = d.tension / (4.0 * f0 * f0 * d.stringMass);
    const double modalMass = d.strings * d.stringMass / 2.0; // the unison moves as one under the hammer
    const double strike = std::clamp (d.strikePosition * s.strikeScale, 0.02, 0.45);

    hammerMass = d.hammerMass;
    // A different felt curve keeps the force at a typical 0.5 mm compression, so it changes how
    // much brighter loud notes get rather than the overall brightness
    exponent = d.hammerExponent + s.exponentOffset;
    stiffness = d.hammerStiffness * std::pow (10.0, s.hardness) * std::pow (5.0e-4, -s.exponentOffset);
    damperT60 = d.damperT60 * s.damperScale;

    // Normal modes per partial: the prompt (in-phase) one, plus detuned aftersound ones
    const int components = std::max (2, d.strings);
    const double offsets[] { 0.0, s.detuneCents * (d.strings == 1 ? 0.3 : 1.0), -0.6 * s.detuneCents };
    const double aftersoundWeight = s.aftersoundGain / std::sqrt ((double) (components - 1));

    const double highLoss = d.highLoss * s.highLossScale;
    const double w1 = twoPi * f0 * std::sqrt (1.0 + b);
    const double b1 = std::max (t60ToSigma / (d.t60 * s.decayScale) - highLoss * w1 * w1, 0.1 * t60ToSigma / d.t60);

    const double maxHz = std::min (0.45 * sampleRate, 18000.0);
    const double voicing = std::pow (10.0, d.voicingDb / 20.0);
    const double dtSub = 1.0 / (sampleRate * substeps);

    modeCount = 0;
    for (int n = 1; modeCount + components <= maxModes; ++n)
    {
        const double fn = n * f0 * std::sqrt (1.0 + b * n * n);
        if (fn >= maxHz)
            break;

        const double shape = std::sin (n * std::numbers::pi * strike);
        const double slope = d.strings * d.tension * n * std::numbers::pi / length * (n % 2 == 0 ? 1.0 : -1.0);
        const double wn = twoPi * fn;
        // The board draws energy fastest from partials where its admittance is highest, around
        // 250 Hz: why a middle C's fundamental fades so quickly at first (measured ~27 dB/s)
        const double octaves = std::log2 (fn / 250.0);
        const double drain = boardDrainSigma * s.boardDrain * std::exp (-octaves * octaves / (2.0 * 0.6 * 0.6));
        const double sigma = b1 + highLoss * wn * wn;

        for (int c = 0; c < components; ++c)
        {
            const double hz = fn * std::exp2 (offsets[c] / 1200.0);
            // The prompt (in-phase) motion drives the bridge and pays the board's drain; the
            // aftersound barely moves it
            const double decay = c == 0 ? sigma + drain : sigma / d.aftersoundRatio;
            const double weight = c == 0 ? 1.0 : aftersoundWeight;
            const double w = twoPi * hz;
            const int m = modeCount++;

            const double r = std::exp (-decay / sampleRate), theta = w / sampleRate;
            pRe[(size_t) m] = (float) (r * std::cos (theta));
            pIm[(size_t) m] = (float) (r * std::sin (theta));
            const double rSub = std::exp (-decay * dtSub), thetaSub = w * dtSub;
            pReSub[(size_t) m] = (float) (rSub * std::cos (thetaSub));
            pImSub[(size_t) m] = (float) (rSub * std::sin (thetaSub));

            // An impulse F dt starts the mode as (F dt shape / M w) e^-sigma t sin wt
            inputGain[(size_t) m] = (float) (weight * shape * dtSub / (modalMass * w));
            promptMode[(size_t) m] = c == 0 ? 1.0f : 0.0f;
            strikeShape[(size_t) m] = (float) shape;
            bridgeGain[(size_t) m] = (float) (slope * outputScale * voicing);
            modeHz[(size_t) m] = hz;
        }
    }

    for (int m = modeCount; m < maxModes; ++m)
    {
        re[(size_t) m] = im[(size_t) m] = 0.0f;
        pRe[(size_t) m] = pIm[(size_t) m] = pReSub[(size_t) m] = pImSub[(size_t) m] = 0.0f;
        inputGain[(size_t) m] = hammerGain[(size_t) m] = promptMode[(size_t) m] = 0.0f;
        strikeShape[(size_t) m] = bridgeGain[(size_t) m] = 0.0f;
    }

    // Sympathetic bank: the low partials, driven through the bridge (the end of the string), so
    // each responds in proportion to its slope there, and rings with the slow aftersound decay
    symCount = 0;
    for (int n = 1; n <= maxSympathetic; ++n)
    {
        const double fn = n * f0 * std::sqrt (1.0 + b * n * n) * std::exp2 (0.5 * s.detuneCents / 1200.0);
        if (fn >= std::min (maxHz, 6000.0))
            break;
        const double wn = twoPi * fn;
        const double decay = (b1 + highLoss * wn * wn) / d.aftersoundRatio;
        const double slope = d.strings * d.tension * n * std::numbers::pi / length * (n % 2 == 0 ? 1.0 : -1.0);
        const double r = std::exp (-decay / sampleRate), theta = wn / sampleRate;
        const auto i = (size_t) symCount++;
        symPRe[i] = (float) (r * std::cos (theta));
        symPIm[i] = (float) (r * std::sin (theta));
        // The bridge moves by Y F / jw under a force F (Y: its admittance, given as `coupling` at
        // run time); a string end displaced by y feels a modal force slope * y. Drive arrives in
        // output units, hence / outputScale.
        symInput[i] = (float) (slope / (outputScale * sampleRate * modalMass * wn * wn));
        symOutput[i] = (float) (slope * outputScale);
    }
    for (int m = symCount; m < maxSympathetic; ++m)
        symRe[(size_t) m] = symIm[(size_t) m] = symPRe[(size_t) m] = symPIm[(size_t) m] = symInput[(size_t) m] = symOutput[(size_t) m] = 0.0f;

    updateDamper();
}

double PianoKey::modeFrequency (int mode) const
{
    return modeHz[(size_t) std::clamp (mode, 0, maxModes - 1)];
}

void PianoKey::strike (double hammerSpeed, bool unaCorda)
{
    // Una corda: the missed string only joins in through the bridge, so less prompt sound and
    // relatively more aftersound; the shifted hammer meets softer, less compacted felt
    const float prompt = unaCorda ? 0.6f : 1.0f, aftersound = unaCorda ? 1.6f : 1.0f;
    for (int m = 0; m < modeCount; ++m)
        hammerGain[(size_t) m] = inputGain[(size_t) m] * (promptMode[(size_t) m] > 0.5f ? prompt : aftersound);
    strikeStiffness = stiffness * (unaCorda ? 0.4 : 1.0);

    // The hammer starts just touching the string wherever it is
    double displacement = 0.0;
    for (int m = 0; m < modeCount; ++m)
        displacement += strikeShape[(size_t) m] * im[(size_t) m];

    hammerPos = displacement;
    hammerVel = std::max (hammerSpeed, 0.01);
    lastCompressionPow = 0.0;
    contactTime = 0.0;
    sinceContact = 0.0;
    hammerActive = true;
    active = true;
    quietBlocks = 0;
}

void PianoKey::setDamper (float amount)
{
    if (amount != damperAmount)
    {
        damperAmount = amount;
        updateDamper();
    }
}

void PianoKey::updateDamper()
{
    // A damper barely touching the strings (half pedal) only slowly drains them
    const double t60 = damperT60 / std::max (1.0e-3, (double) damperAmount * damperAmount);
    damperTarget = damperAmount > 0.0f && damperT60 > 0.0 ? (float) std::exp (-t60ToSigma / (t60 * sampleRate)) : 1.0f;
    if (! active && ! sympatheticActive)
        damperGain = damperTarget;
}

void PianoKey::process (float* out, int numSamples)
{
    // The damper felt lands (or lifts) over about 15 ms
    if (damperGain != damperTarget)
    {
        const float ramp = (float) (1.0 - std::exp (-numSamples / (0.015 * sampleRate)));
        damperGain += (damperTarget - damperGain) * ramp;
        if (std::abs (damperGain - damperTarget) < 1.0e-9f)
            damperGain = damperTarget;
    }

    if (! active)
    {
        std::fill (ownOutput.begin(), ownOutput.begin() + numSamples, 0.0f);
        return;
    }

    std::fill (ownOutput.begin(), ownOutput.begin() + numSamples, 0.0f);
    hammerInLastBlock = hammerActive;
    if (hammerActive)
        processHammer (ownOutput.data(), numSamples);
    else
        processFree (ownOutput.data(), numSamples);

    // Never let a numerical accident through (to the output, the board or other strings):
    // silence the key instead
    const float e = energy();
    if (! std::isfinite (e) || ! std::isfinite (hammerVel))
    {
        std::fill (ownOutput.begin(), ownOutput.begin() + numSamples, 0.0f);
        std::fill (hammerForce.begin(), hammerForce.begin() + numSamples, 0.0f);
        hammerInLastBlock = false;
        reset();
        return;
    }
    for (int i = 0; i < numSamples; ++i)
        out[i] += ownOutput[(size_t) i];

    // Silent for a while (by energy, so a beat's null doesn't count): stop
    if (! hammerActive && e < silentEnergy)
    {
        if (++quietBlocks > 8)
            reset();
    }
    else
    {
        quietBlocks = 0;
    }
}

void PianoKey::processHammer (float* out, int numSamples)
{
    const double dtSub = 1.0 / (sampleRate * substeps);
    const int n = modeCount;
    float* const sRe = re.data();
    float* const sIm = im.data();
    const float* const aRe = pReSub.data();
    const float* const aIm = pImSub.data();
    const float* const gain = hammerGain.data();
    const float* const shape = strikeShape.data();
    const float* const bridge = bridgeGain.data();

    for (int i = 0; i < numSamples; ++i)
    {
        float meanForce = 0.0f;
        for (int step = 0; step < substeps; ++step)
        {
            float displacement = 0.0f;
#pragma omp simd reduction(+ : displacement)
            for (int m = 0; m < n; ++m)
                displacement += shape[m] * sIm[m];

            // Felt compression -> force, with hysteresis (the felt pushes back harder while
            // being compressed than while relaxing). Very stiff felt (hard voicing at the top,
            // fortissimo) can ring faster than a substep can follow, so the hammer then takes
            // smaller steps of its own against the string's displacement, keeping its explicit
            // integration stable; the modes get the mean force.
            int micro = 1;
            if (const double c = hammerPos - displacement; c > 0.0)
            {
                const double keff = exponent * strikeStiffness * std::pow (c, exponent - 1.0);
                micro = std::clamp ((int) std::ceil (dtSub * std::sqrt (keff / hammerMass) / 0.3), 1, 64);
            }
            const double dt = dtSub / micro;
            double force = 0.0, compression = 0.0;
            for (int k = 0; k < micro; ++k)
            {
                compression = hammerPos - displacement;
                const double compressionPow = compression > 0.0 ? std::pow (compression, exponent) : 0.0;
                const double f = std::max (0.0, strikeStiffness * (compressionPow + hysteresis * (compressionPow - lastCompressionPow) / dt));
                lastCompressionPow = compressionPow;
                hammerVel -= f / hammerMass * dt;
                hammerPos += hammerVel * dt;
                force += f / micro;
            }
            contactTime += dtSub;
            sinceContact = compression > 0.0 ? 0.0 : sinceContact + dtSub;

            const float f = (float) force;
            meanForce += f / substeps;
#pragma omp simd
            for (int m = 0; m < n; ++m)
            {
                const float r = aRe[m] * sRe[m] - aIm[m] * sIm[m] + gain[m] * f;
                sIm[m] = aRe[m] * sIm[m] + aIm[m] * sRe[m];
                sRe[m] = r;
            }
        }

        hammerForce[(size_t) i] = meanForce;
        float sum = 0.0f;
        const float dg = damperGain;
#pragma omp simd reduction(+ : sum)
        for (int m = 0; m < n; ++m)
        {
            sRe[m] *= dg;
            sIm[m] *= dg;
            sum += bridge[m] * sIm[m];
        }
        out[i] += sum;
    }

    // The hammer has rebounded and is falling away (bass hammers can touch more than once)
    if ((contactTime > 0.002 && sinceContact > 0.004 && hammerVel < 0.0) || contactTime > 0.05)
        hammerActive = false;
}

void PianoKey::processFree (float* out, int numSamples)
{
    const int n = modeCount;
    float* const sRe = re.data();
    float* const sIm = im.data();
    const float* const aRe = pRe.data();
    const float* const aIm = pIm.data();
    const float* const bridge = bridgeGain.data();
    const float dg = damperGain;

    for (int i = 0; i < numSamples; ++i)
    {
        float sum = 0.0f;
#pragma omp simd reduction(+ : sum)
        for (int m = 0; m < n; ++m)
        {
            const float r = (aRe[m] * sRe[m] - aIm[m] * sIm[m]) * dg;
            const float s = (aRe[m] * sIm[m] + aIm[m] * sRe[m]) * dg;
            sRe[m] = r;
            sIm[m] = s;
            sum += bridge[m] * s;
        }
        out[i] += sum;
    }
}

void PianoKey::processSympathetic (const float* bridge, float* out, int numSamples, float coupling)
{
    const bool undamped = damperAmount < 1.0f || damperT60 <= 0.0;
    if (! undamped && ! sympatheticActive)
        return;

    float* const sRe = symRe.data();
    float* const sIm = symIm.data();
    const float* const aRe = symPRe.data();
    const float* const aIm = symPIm.data();
    const float* const input = symInput.data();
    const float* const output = symOutput.data();
    const int n = symCount;
    const float dg = damperGain;
    // A resting damper keeps the strings from being driven
    const float drive = damperT60 <= 0.0 ? coupling : coupling * (1.0f - damperAmount);

    float peakDrive = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        const float x = drive * (bridge[i] - ownOutput[(size_t) i]);
        peakDrive = std::max (peakDrive, std::abs (x));
        float sum = 0.0f;
#pragma omp simd reduction(+ : sum)
        for (int m = 0; m < n; ++m)
        {
            const float r = (aRe[m] * sRe[m] - aIm[m] * sIm[m]) * dg + input[m] * x;
            const float s = (aRe[m] * sIm[m] + aIm[m] * sRe[m]) * dg;
            sRe[m] = r;
            sIm[m] = s;
            sum += output[m] * s;
        }
        out[i] += sum;
    }

    // Keep running while driven or still ringing
    sympatheticActive = peakDrive > 0.0f || sympatheticEnergy() > silentEnergy;
    if (! sympatheticActive)
    {
        symRe.fill (0.0f);
        symIm.fill (0.0f);
    }
}

float PianoKey::sympatheticEnergy() const
{
    float e = 0.0f;
    for (int m = 0; m < symCount; ++m)
        e += symOutput[(size_t) m] * symOutput[(size_t) m] * (symRe[(size_t) m] * symRe[(size_t) m] + symIm[(size_t) m] * symIm[(size_t) m]);
    return e;
}

float PianoKey::energy() const
{
    float e = 0.0f;
    for (int m = 0; m < modeCount; ++m)
        e += bridgeGain[(size_t) m] * bridgeGain[(size_t) m] * (re[(size_t) m] * re[(size_t) m] + im[(size_t) m] * im[(size_t) m]);
    return e;
}

} // namespace dpiano
