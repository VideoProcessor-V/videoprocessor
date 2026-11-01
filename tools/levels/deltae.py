# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""CIEDE2000 (Sharma, Wu, Dalal 2005) and delta E ITP (ITU-R BT.2124)."""
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import expected as ex  # noqa: E402


def xyz_to_lab(xyz, white):
    """xyz and white in the same units; returns (L, a, b)."""
    t = np.asarray(xyz, dtype=np.float64) / np.asarray(white, dtype=np.float64)
    d = 6.0 / 29.0
    f = np.where(t > d ** 3, np.cbrt(np.maximum(t, 0.0)), t / (3 * d * d) + 4.0 / 29.0)
    return np.stack([116.0 * f[..., 1] - 16.0, 500.0 * (f[..., 0] - f[..., 1]),
                     200.0 * (f[..., 1] - f[..., 2])], axis=-1)


def ciede2000(lab1, lab2, kl=1.0, kc=1.0, kh=1.0):
    """Scalar CIEDE2000 for two (L, a, b) triples."""
    l1, a1, b1 = (float(v) for v in lab1)
    l2, a2, b2 = (float(v) for v in lab2)
    c1 = math.hypot(a1, b1)
    c2 = math.hypot(a2, b2)
    cbar = (c1 + c2) / 2.0
    g = 0.5 * (1.0 - math.sqrt(cbar ** 7 / (cbar ** 7 + 25.0 ** 7)))
    a1p, a2p = (1.0 + g) * a1, (1.0 + g) * a2
    c1p, c2p = math.hypot(a1p, b1), math.hypot(a2p, b2)
    h1p = math.degrees(math.atan2(b1, a1p)) % 360.0 if (b1 or a1p) else 0.0
    h2p = math.degrees(math.atan2(b2, a2p)) % 360.0 if (b2 or a2p) else 0.0
    dlp = l2 - l1
    dcp = c2p - c1p
    if c1p * c2p == 0.0:
        dhp = 0.0
    else:
        dh = h2p - h1p
        if dh > 180.0:
            dh -= 360.0
        elif dh < -180.0:
            dh += 360.0
        dhp = dh
    dHp = 2.0 * math.sqrt(c1p * c2p) * math.sin(math.radians(dhp / 2.0))
    lbp = (l1 + l2) / 2.0
    cbp = (c1p + c2p) / 2.0
    if c1p * c2p == 0.0:
        hbp = h1p + h2p
    elif abs(h1p - h2p) <= 180.0:
        hbp = (h1p + h2p) / 2.0
    elif h1p + h2p < 360.0:
        hbp = (h1p + h2p + 360.0) / 2.0
    else:
        hbp = (h1p + h2p - 360.0) / 2.0
    t = (1.0 - 0.17 * math.cos(math.radians(hbp - 30.0)) + 0.24 * math.cos(math.radians(2.0 * hbp))
         + 0.32 * math.cos(math.radians(3.0 * hbp + 6.0)) - 0.20 * math.cos(math.radians(4.0 * hbp - 63.0)))
    dtheta = 30.0 * math.exp(-(((hbp - 275.0) / 25.0) ** 2))
    rc = 2.0 * math.sqrt(cbp ** 7 / (cbp ** 7 + 25.0 ** 7))
    sl = 1.0 + 0.015 * (lbp - 50.0) ** 2 / math.sqrt(20.0 + (lbp - 50.0) ** 2)
    sc = 1.0 + 0.045 * cbp
    sh = 1.0 + 0.015 * cbp * t
    rt = -math.sin(math.radians(2.0 * dtheta)) * rc
    x = dlp / (kl * sl)
    y = dcp / (kc * sc)
    z = dHp / (kh * sh)
    return math.sqrt(x * x + y * y + z * z + rt * y * z)


def d65_white_xyz(y=100.0):
    wx, wy = ex.D65
    return np.array([wx / wy, 1.0, (1.0 - wx - wy) / wy]) * y


def delta_e_2000_xyz(xyz1, xyz2, white_y):
    """SDR: XYZ in nits; normalised so the display white has Y = 100."""
    k = 100.0 / white_y
    wh = d65_white_xyz(100.0)
    return ciede2000(xyz_to_lab(np.asarray(xyz1) * k, wh), xyz_to_lab(np.asarray(xyz2) * k, wh))


# ---- ITP ---------------------------------------------------------------------

_RGB2LMS = np.array([[1688, 2146, 262], [683, 2951, 462], [99, 309, 3688]], dtype=np.float64) / 4096.0
_LMS2ICTCP = np.array([[2048, 2048, 0], [6610, -13613, 7003], [17933, -17390, -543]],
                      dtype=np.float64) / 4096.0
_XYZ2RGB2020 = np.linalg.inv(ex.rgb_to_xyz_matrix("bt2020nc"))


def itp_from_rgb2020(rgb_nits):
    """Linear BT.2020 RGB in absolute nits -> (I, T, P)."""
    lms = np.asarray(rgb_nits, dtype=np.float64) @ _RGB2LMS.T
    lmsp = ex.pq_inverse(lms)
    ictcp = lmsp @ _LMS2ICTCP.T
    return np.array([ictcp[..., 0], 0.5 * ictcp[..., 1], ictcp[..., 2]])


def itp_from_xyz(xyz):
    return itp_from_rgb2020(np.asarray(xyz, dtype=np.float64) @ _XYZ2RGB2020.T)


def delta_e_itp_xyz(xyz1, xyz2):
    d = itp_from_xyz(xyz1) - itp_from_xyz(xyz2)
    return float(720.0 * math.sqrt(float(np.sum(d * d))))


# Sharma 2005, Table 1: (Lab1, Lab2, dE00)
SHARMA_PAIRS = [
    ((50.0000, 2.6772, -79.7751), (50.0000, 0.0000, -82.7485), 2.0425),
    ((50.0000, 3.1571, -77.2803), (50.0000, 0.0000, -82.7485), 2.8615),
    ((50.0000, 2.8361, -74.0200), (50.0000, 0.0000, -82.7485), 3.4412),
    ((50.0000, -1.3802, -84.2814), (50.0000, 0.0000, -82.7485), 1.0000),
    ((50.0000, -1.1848, -84.8006), (50.0000, 0.0000, -82.7485), 1.0000),
    ((50.0000, -0.9009, -85.5211), (50.0000, 0.0000, -82.7485), 1.0000),
    ((50.0000, 0.0000, 0.0000), (50.0000, -1.0000, 2.0000), 2.3669),
    ((50.0000, -1.0000, 2.0000), (50.0000, 0.0000, 0.0000), 2.3669),
    ((50.0000, 2.4900, -0.0010), (50.0000, -2.4900, 0.0009), 7.1792),
    ((50.0000, 2.4900, -0.0010), (50.0000, -2.4900, 0.0010), 7.1792),
    ((50.0000, 2.4900, -0.0010), (50.0000, -2.4900, 0.0011), 7.2195),
    ((50.0000, 2.4900, -0.0010), (50.0000, -2.4900, 0.0012), 7.2195),
    ((50.0000, -0.0010, 2.4900), (50.0000, 0.0009, -2.4900), 4.8045),
    ((50.0000, -0.0010, 2.4900), (50.0000, 0.0010, -2.4900), 4.8045),
    ((50.0000, -0.0010, 2.4900), (50.0000, 0.0011, -2.4900), 4.7461),
    ((50.0000, 2.5000, 0.0000), (50.0000, 0.0000, -2.5000), 4.3065),
    ((50.0000, 2.5000, 0.0000), (73.0000, 25.0000, -18.0000), 27.1492),
    ((50.0000, 2.5000, 0.0000), (61.0000, -5.0000, 29.0000), 22.8977),
    ((50.0000, 2.5000, 0.0000), (56.0000, -27.0000, -3.0000), 31.9030),
    ((50.0000, 2.5000, 0.0000), (58.0000, 24.0000, 15.0000), 19.4535),
    ((50.0000, 2.5000, 0.0000), (50.0000, 3.1736, 0.5854), 1.0000),
    ((50.0000, 2.5000, 0.0000), (50.0000, 3.2972, 0.0000), 1.0000),
    ((50.0000, 2.5000, 0.0000), (50.0000, 1.8634, 0.5757), 1.0000),
    ((50.0000, 2.5000, 0.0000), (50.0000, 3.2592, 0.3350), 1.0000),
    ((60.2574, -34.0099, 36.2677), (60.4626, -34.1751, 39.4387), 1.2644),
    ((63.0109, -31.0961, -5.8663), (62.8187, -29.7946, -4.0864), 1.2630),
    ((61.2901, 3.7196, -5.3901), (61.4292, 2.2480, -4.9620), 1.8731),
    ((35.0831, -44.1164, 3.7933), (35.0232, -40.0716, 1.5901), 1.8645),
    ((22.7233, 20.0904, -46.6940), (23.0331, 14.9730, -42.5619), 2.0373),
    ((36.4612, 47.8580, 18.3852), (36.2715, 50.5065, 21.2231), 1.4146),
    ((90.8027, -2.0831, 1.4410), (91.1528, -1.6435, 0.0447), 1.4441),
    ((90.9257, -0.5406, -0.9208), (88.6381, -0.8985, -0.7239), 1.5381),
    ((6.7747, -0.2908, -2.4247), (5.8714, -0.0985, -2.2286), 0.6377),
    ((2.0776, 0.0795, -1.1350), (0.9033, -0.0636, -0.5514), 0.9082),
]
