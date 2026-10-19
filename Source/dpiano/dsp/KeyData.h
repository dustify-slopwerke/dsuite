#pragma once

#include <array>

namespace dpiano
{

constexpr int lowestKey = 21, highestKey = 108, numKeys = highestKey - lowestKey + 1; // A0 .. C8

// Physical description of one key of a concert grand: its strings and its hammer. Values follow
// published measurements (Chaigne & Askenfelt 1994, Conklin 1996, Fletcher & Rossing), smoothly
// interpolated between reference keys.
struct KeyData
{
    int key = 60;                  // MIDI note
    int strings = 3;               // unison strings: 1 in the low bass, 2 in the bass, 3 above
    double inharmonicity = 4e-4;   // B in f_n = n f0 sqrt (1 + B n^2)
    double stringMass = 4e-3;      // kg, one string's speaking length
    double tension = 700.0;        // N, per string
    double hammerMass = 3e-3;      // kg, effective
    double hammerStiffness = 4e9;  // K in F = K x^p, N/m^p
    double hammerExponent = 2.5;   // p
    double strikePosition = 0.12;  // fraction of the speaking length from the agraffe
    double t60 = 7.0;              // s, prompt decay of the fundamental
    double highLoss = 2.7e-9;      // b3 in sigma = b1 + b3 w^2 (s): frequency-dependent loss
    double aftersoundRatio = 4.0;  // aftersound decay time / prompt decay time
    double damperT60 = 0.3;        // s with the damper down; 0 = no damper (top of the keyboard)
    double knock = 0.1;            // relative level of the hammer knock sent straight to the board
    double voicingDb = 0.0;        // the technician's evening-out of loudness along the scale
};

KeyData keyData (int key);

// Stretched tuning: each key's offset from equal temperament in cents (A4 = 0), with octaves as an
// aural tuner sets them, beatless on 4:2 (6:3 in the bass) given the strings' inharmonicity.
// `inharmonicityScale` scales every key's B.
std::array<double, numKeys> stretchCents (double inharmonicityScale);

} // namespace dpiano
