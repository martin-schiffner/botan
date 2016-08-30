/*
* NEWHOPE Ring-LWE scheme
* Based on the public domain reference implementation by the
* designers (https://github.com/tpoeppelmann/newhope)
*
* Further changes
* (C) 2016 Jack Lloyd
*
* Botan is released under the Simplified BSD License (see license.txt)
*/

#include <botan/newhope.h>
#include <botan/keccak.h>
#include <botan/loadstor.h>

namespace Botan {

typedef newhope_poly poly;

// Don't change this :)
#define PARAM_Q 12289
#define PARAM_N 1024

#define NEWHOPE_POLY_BYTES 1792
#define NEWHOPE_SEED_BYTES 32

#define SHAKE128_RATE 168

namespace {

/* Incomplete-reduction routines; for details on allowed input ranges
 * and produced output ranges, see the description in the paper:
 * https://cryptojedi.org/papers/#newhope */

inline uint16_t montgomery_reduce(uint32_t a)
{
  const uint32_t qinv = 12287; // -inverse_mod(p,2^18)
  const uint32_t rlog = 18;

  uint32_t u;

  u = (a * qinv);
  u &= ((1<<rlog)-1);
  u *= PARAM_Q;
  a = a + u;
  return a >> 18;
}

inline uint16_t barrett_reduce(uint16_t a)
{
  uint32_t u;

  u = ((uint32_t) a * 5) >> 16;
  u *= PARAM_Q;
  a -= u;
  return a;
}

inline void mul_coefficients(uint16_t* poly, const uint16_t* factors)
{
    unsigned int i;

    for(i = 0; i < PARAM_N; i++)
      poly[i] = montgomery_reduce((poly[i] * factors[i]));
}

/* GS_bo_to_no; omegas need to be in Montgomery domain */
inline void ntt(uint16_t * a, const uint16_t* omega)
{
  int i, start, j, jTwiddle, distance;
  uint16_t temp, W;


  for(i=0;i<10;i+=2)
  {
    // Even level
    distance = (1<<i);
    for(start = 0; start < distance;start++)
    {
      jTwiddle = 0;
      for(j=start;j<PARAM_N-1;j+=2*distance)
      {
        W = omega[jTwiddle++];
        temp = a[j];
        a[j] = (temp + a[j + distance]); // Omit reduction (be lazy)
        a[j + distance] = montgomery_reduce((W * ((uint32_t)temp + 3*PARAM_Q - a[j + distance])));
      }
    }

    // Odd level
    distance <<= 1;
    for(start = 0; start < distance;start++)
    {
      jTwiddle = 0;
      for(j=start;j<PARAM_N-1;j+=2*distance)
      {
        W = omega[jTwiddle++];
        temp = a[j];
        a[j] = barrett_reduce((temp + a[j + distance]));
        a[j + distance] = montgomery_reduce((W * ((uint32_t)temp + 3*PARAM_Q - a[j + distance])));
      }
    }
  }
}

inline void poly_frombytes(poly *r, const unsigned char *a)
{
  int i;
  for(i=0;i<PARAM_N/4;i++)
  {
    r->coeffs[4*i+0] =                               a[7*i+0]        | (((uint16_t)a[7*i+1] & 0x3f) << 8);
    r->coeffs[4*i+1] = (a[7*i+1] >> 6) | (((uint16_t)a[7*i+2]) << 2) | (((uint16_t)a[7*i+3] & 0x0f) << 10);
    r->coeffs[4*i+2] = (a[7*i+3] >> 4) | (((uint16_t)a[7*i+4]) << 4) | (((uint16_t)a[7*i+5] & 0x03) << 12);
    r->coeffs[4*i+3] = (a[7*i+5] >> 2) | (((uint16_t)a[7*i+6]) << 6);
  }
}

inline void poly_tobytes(unsigned char *r, const poly *p)
{
  int i;
  uint16_t t0,t1,t2,t3,m;
  int16_t c;
  for(i=0;i<PARAM_N/4;i++)
  {
    t0 = barrett_reduce(p->coeffs[4*i+0]); //Make sure that coefficients have only 14 bits
    t1 = barrett_reduce(p->coeffs[4*i+1]);
    t2 = barrett_reduce(p->coeffs[4*i+2]);
    t3 = barrett_reduce(p->coeffs[4*i+3]);

    m = t0 - PARAM_Q;
    c = m;
    c >>= 15;
    t0 = m ^ ((t0^m)&c); // <Make sure that coefficients are in [0,q]

    m = t1 - PARAM_Q;
    c = m;
    c >>= 15;
    t1 = m ^ ((t1^m)&c); // <Make sure that coefficients are in [0,q]

    m = t2 - PARAM_Q;
    c = m;
    c >>= 15;
    t2 = m ^ ((t2^m)&c); // <Make sure that coefficients are in [0,q]

    m = t3 - PARAM_Q;
    c = m;
    c >>= 15;
    t3 = m ^ ((t3^m)&c); // <Make sure that coefficients are in [0,q]

    r[7*i+0] =  t0 & 0xff;
    r[7*i+1] = (t0 >> 8) | (t1 << 6);
    r[7*i+2] = (t1 >> 2);
    r[7*i+3] = (t1 >> 10) | (t2 << 4);
    r[7*i+4] = (t2 >> 4);
    r[7*i+5] = (t2 >> 12) | (t3 << 2);
    r[7*i+6] = (t3 >> 6);
  }
}

inline void poly_getnoise(Botan::RandomNumberGenerator& rng, poly *r)
{
  unsigned char buf[4*PARAM_N];
  uint32_t *tp, t,d, a, b;
  int i,j;

  // Not an endian problem because this is just used for RNG output
  // Is an endian problem for tests
  tp = (uint32_t *) buf;

  rng.randomize(buf, 4*PARAM_N);

  for(i=0;i<PARAM_N;i++)
  {
    t = tp[i];
    d = 0;
    for(j=0;j<8;j++)
      d += (t >> j) & 0x01010101;
    a = ((d >> 8) & 0xff) + (d & 0xff);
    b = (d >> 24) + ((d >> 16) & 0xff);
    r->coeffs[i] = a + PARAM_Q - b;
  }
}

inline void poly_pointwise(poly *r, const poly *a, const poly *b)
{
  int i;
  uint16_t t;
  for(i=0;i<PARAM_N;i++)
  {
    t       = montgomery_reduce(3186*b->coeffs[i]); /* t is now in Montgomery domain */
    r->coeffs[i] = montgomery_reduce(a->coeffs[i] * t); /* r->coeffs[i] is back in normal domain */
  }
}

inline void poly_add(poly *r, const poly *a, const poly *b)
{
  int i;
  for(i=0;i<PARAM_N;i++)
    r->coeffs[i] = barrett_reduce(a->coeffs[i] + b->coeffs[i]);
}

inline void poly_ntt(poly *r)
{

static const uint16_t omegas_montgomery[PARAM_N/2] = {4075,6974,7373,7965,3262,5079,522,2169,6364,1018,1041,8775,2344,11011,5574,1973,4536,1050,6844,3860,3818,6118,2683,1190,4789,7822,7540,6752,5456,4449,3789,12142,11973,382,3988,468,6843,5339,6196,3710,11316,1254,5435,10930,3998,10256,10367,3879,11889,1728,6137,4948,5862,6136,3643,6874,8724,654,10302,1702,7083,6760,56,3199,9987,605,11785,8076,5594,9260,6403,4782,6212,4624,9026,8689,4080,11868,6221,3602,975,8077,8851,9445,5681,3477,1105,142,241,12231,1003,3532,5009,1956,6008,11404,7377,2049,10968,12097,7591,5057,3445,4780,2920,7048,3127,8120,11279,6821,11502,8807,12138,2127,2839,3957,431,1579,6383,9784,5874,677,3336,6234,2766,1323,9115,12237,2031,6956,6413,2281,3969,3991,12133,9522,4737,10996,4774,5429,11871,3772,453,5908,2882,1805,2051,1954,11713,3963,2447,6142,8174,3030,1843,2361,12071,2908,3529,3434,3202,7796,2057,5369,11939,1512,6906,10474,11026,49,10806,5915,1489,9789,5942,10706,10431,7535,426,8974,3757,10314,9364,347,5868,9551,9634,6554,10596,9280,11566,174,2948,2503,6507,10723,11606,2459,64,3656,8455,5257,5919,7856,1747,9166,5486,9235,6065,835,3570,4240,11580,4046,10970,9139,1058,8210,11848,922,7967,1958,10211,1112,3728,4049,11130,5990,1404,325,948,11143,6190,295,11637,5766,8212,8273,2919,8527,6119,6992,8333,1360,2555,6167,1200,7105,7991,3329,9597,12121,5106,5961,10695,10327,3051,9923,4896,9326,81,3091,1000,7969,4611,726,1853,12149,4255,11112,2768,10654,1062,2294,3553,4805,2747,4846,8577,9154,1170,2319,790,11334,9275,9088,1326,5086,9094,6429,11077,10643,3504,3542,8668,9744,1479,1,8246,7143,11567,10984,4134,5736,4978,10938,5777,8961,4591,5728,6461,5023,9650,7468,949,9664,2975,11726,2744,9283,10092,5067,12171,2476,3748,11336,6522,827,9452,5374,12159,7935,3296,3949,9893,4452,10908,2525,3584,8112,8011,10616,4989,6958,11809,9447,12280,1022,11950,9821,11745,5791,5092,2089,9005,2881,3289,2013,9048,729,7901,1260,5755,4632,11955,2426,10593,1428,4890,5911,3932,9558,8830,3637,5542,145,5179,8595,3707,10530,355,3382,4231,9741,1207,9041,7012,1168,10146,11224,4645,11885,10911,10377,435,7952,4096,493,9908,6845,6039,2422,2187,9723,8643,9852,9302,6022,7278,1002,4284,5088,1607,7313,875,8509,9430,1045,2481,5012,7428,354,6591,9377,11847,2401,1067,7188,11516,390,8511,8456,7270,545,8585,9611,12047,1537,4143,4714,4885,1017,5084,1632,3066,27,1440,8526,9273,12046,11618,9289,3400,9890,3136,7098,8758,11813,7384,3985,11869,6730,10745,10111,2249,4048,2884,11136,2126,1630,9103,5407,2686,9042,2969,8311,9424,9919,8779,5332,10626,1777,4654,10863,7351,3636,9585,5291,8374,2166,4919,12176,9140,12129,7852,12286,4895,10805,2780,5195,2305,7247,9644,4053,10600,3364,3271,4057,4414,9442,7917,2174};

        static const uint16_t psis_bitrev_montgomery[PARAM_N] = {4075,6974,7373,7965,3262,5079,522,2169,6364,1018,1041,8775,2344,11011,5574,1973,4536,1050,6844,3860,3818,6118,2683,1190,4789,7822,7540,6752,5456,4449,3789,12142,11973,382,3988,468,6843,5339,6196,3710,11316,1254,5435,10930,3998,10256,10367,3879,11889,1728,6137,4948,5862,6136,3643,6874,8724,654,10302,1702,7083,6760,56,3199,9987,605,11785,8076,5594,9260,6403,4782,6212,4624,9026,8689,4080,11868,6221,3602,975,8077,8851,9445,5681,3477,1105,142,241,12231,1003,3532,5009,1956,6008,11404,7377,2049,10968,12097,7591,5057,3445,4780,2920,7048,3127,8120,11279,6821,11502,8807,12138,2127,2839,3957,431,1579,6383,9784,5874,677,3336,6234,2766,1323,9115,12237,2031,6956,6413,2281,3969,3991,12133,9522,4737,10996,4774,5429,11871,3772,453,5908,2882,1805,2051,1954,11713,3963,2447,6142,8174,3030,1843,2361,12071,2908,3529,3434,3202,7796,2057,5369,11939,1512,6906,10474,11026,49,10806,5915,1489,9789,5942,10706,10431,7535,426,8974,3757,10314,9364,347,5868,9551,9634,6554,10596,9280,11566,174,2948,2503,6507,10723,11606,2459,64,3656,8455,5257,5919,7856,1747,9166,5486,9235,6065,835,3570,4240,11580,4046,10970,9139,1058,8210,11848,922,7967,1958,10211,1112,3728,4049,11130,5990,1404,325,948,11143,6190,295,11637,5766,8212,8273,2919,8527,6119,6992,8333,1360,2555,6167,1200,7105,7991,3329,9597,12121,5106,5961,10695,10327,3051,9923,4896,9326,81,3091,1000,7969,4611,726,1853,12149,4255,11112,2768,10654,1062,2294,3553,4805,2747,4846,8577,9154,1170,2319,790,11334,9275,9088,1326,5086,9094,6429,11077,10643,3504,3542,8668,9744,1479,1,8246,7143,11567,10984,4134,5736,4978,10938,5777,8961,4591,5728,6461,5023,9650,7468,949,9664,2975,11726,2744,9283,10092,5067,12171,2476,3748,11336,6522,827,9452,5374,12159,7935,3296,3949,9893,4452,10908,2525,3584,8112,8011,10616,4989,6958,11809,9447,12280,1022,11950,9821,11745,5791,5092,2089,9005,2881,3289,2013,9048,729,7901,1260,5755,4632,11955,2426,10593,1428,4890,5911,3932,9558,8830,3637,5542,145,5179,8595,3707,10530,355,3382,4231,9741,1207,9041,7012,1168,10146,11224,4645,11885,10911,10377,435,7952,4096,493,9908,6845,6039,2422,2187,9723,8643,9852,9302,6022,7278,1002,4284,5088,1607,7313,875,8509,9430,1045,2481,5012,7428,354,6591,9377,11847,2401,1067,7188,11516,390,8511,8456,7270,545,8585,9611,12047,1537,4143,4714,4885,1017,5084,1632,3066,27,1440,8526,9273,12046,11618,9289,3400,9890,3136,7098,8758,11813,7384,3985,11869,6730,10745,10111,2249,4048,2884,11136,2126,1630,9103,5407,2686,9042,2969,8311,9424,9919,8779,5332,10626,1777,4654,10863,7351,3636,9585,5291,8374,2166,4919,12176,9140,12129,7852,12286,4895,10805,2780,5195,2305,7247,9644,4053,10600,3364,3271,4057,4414,9442,7917,2174,3947,11951,2455,6599,10545,10975,3654,2894,7681,7126,7287,12269,4119,3343,2151,1522,7174,7350,11041,2442,2148,5959,6492,8330,8945,5598,3624,10397,1325,6565,1945,11260,10077,2674,3338,3276,11034,506,6505,1392,5478,8778,1178,2776,3408,10347,11124,2575,9489,12096,6092,10058,4167,6085,923,11251,11912,4578,10669,11914,425,10453,392,10104,8464,4235,8761,7376,2291,3375,7954,8896,6617,7790,1737,11667,3982,9342,6680,636,6825,7383,512,4670,2900,12050,7735,994,1687,11883,7021,146,10485,1403,5189,6094,2483,2054,3042,10945,3981,10821,11826,8882,8151,180,9600,7684,5219,10880,6780,204,11232,2600,7584,3121,3017,11053,7814,7043,4251,4739,11063,6771,7073,9261,2360,11925,1928,11825,8024,3678,3205,3359,11197,5209,8581,3238,8840,1136,9363,1826,3171,4489,7885,346,2068,1389,8257,3163,4840,6127,8062,8921,612,4238,10763,8067,125,11749,10125,5416,2110,716,9839,10584,11475,11873,3448,343,1908,4538,10423,7078,4727,1208,11572,3589,2982,1373,1721,10753,4103,2429,4209,5412,5993,9011,438,3515,7228,1218,8347,5232,8682,1327,7508,4924,448,1014,10029,12221,4566,5836,12229,2717,1535,3200,5588,5845,412,5102,7326,3744,3056,2528,7406,8314,9202,6454,6613,1417,10032,7784,1518,3765,4176,5063,9828,2275,6636,4267,6463,2065,7725,3495,8328,8755,8144,10533,5966,12077,9175,9520,5596,6302,8400,579,6781,11014,5734,11113,11164,4860,1131,10844,9068,8016,9694,3837,567,9348,7000,6627,7699,5082,682,11309,5207,4050,7087,844,7434,3769,293,9057,6940,9344,10883,2633,8190,3944,5530,5604,3480,2171,9282,11024,2213,8136,3805,767,12239,216,11520,6763,10353,7,8566,845,7235,3154,4360,3285,10268,2832,3572,1282,7559,3229,8360,10583,6105,3120,6643,6203,8536,8348,6919,3536,9199,10891,11463,5043,1658,5618,8787,5789,4719,751,11379,6389,10783,3065,7806,6586,2622,5386,510,7628,6921,578,10345,11839,8929,4684,12226,7154,9916,7302,8481,3670,11066,2334,1590,7878,10734,1802,1891,5103,6151,8820,3418,7846,9951,4693,417,9996,9652,4510,2946,5461,365,881,1927,1015,11675,11009,1371,12265,2485,11385,5039,6742,8449,1842,12217,8176,9577,4834,7937,9461,2643,11194,3045,6508,4094,3451,7911,11048,5406,4665,3020,6616,11345,7519,3669,5287,1790,7014,5410,11038,11249,2035,6125,10407,4565,7315,5078,10506,2840,2478,9270,4194,9195,4518,7469,1160,6878,2730,10421,10036,1734,3815,10939,5832,10595,10759,4423,8420,9617,7119,11010,11424,9173,189,10080,10526,3466,10588,7592,3578,11511,7785,9663,530,12150,8957,2532,3317,9349,10243,1481,9332,3454,3758,7899,4218,2593,11410,2276,982,6513,1849,8494,9021,4523,7988,8,457,648,150,8000,2307,2301,874,5650,170,9462,2873,9855,11498,2535,11169,5808,12268,9687,1901,7171,11787,3846,1573,6063,3793,466,11259,10608,3821,6320,4649,6263,2929};

  mul_coefficients(r->coeffs, psis_bitrev_montgomery);
  ntt((uint16_t *)r->coeffs, omegas_montgomery);
}

inline void bitrev_vector(uint16_t* poly)
{
static const uint16_t bitrev_table[1024] = {
  0,512,256,768,128,640,384,896,64,576,320,832,192,704,448,960,32,544,288,800,160,672,416,928,96,608,352,864,224,736,480,992,
  16,528,272,784,144,656,400,912,80,592,336,848,208,720,464,976,48,560,304,816,176,688,432,944,112,624,368,880,240,752,496,1008,
  8,520,264,776,136,648,392,904,72,584,328,840,200,712,456,968,40,552,296,808,168,680,424,936,104,616,360,872,232,744,488,1000,
  24,536,280,792,152,664,408,920,88,600,344,856,216,728,472,984,56,568,312,824,184,696,440,952,120,632,376,888,248,760,504,1016,
  4,516,260,772,132,644,388,900,68,580,324,836,196,708,452,964,36,548,292,804,164,676,420,932,100,612,356,868,228,740,484,996,
  20,532,276,788,148,660,404,916,84,596,340,852,212,724,468,980,52,564,308,820,180,692,436,948,116,628,372,884,244,756,500,1012,
  12,524,268,780,140,652,396,908,76,588,332,844,204,716,460,972,44,556,300,812,172,684,428,940,108,620,364,876,236,748,492,1004,
  28,540,284,796,156,668,412,924,92,604,348,860,220,732,476,988,60,572,316,828,188,700,444,956,124,636,380,892,252,764,508,1020,
  2,514,258,770,130,642,386,898,66,578,322,834,194,706,450,962,34,546,290,802,162,674,418,930,98,610,354,866,226,738,482,994,
  18,530,274,786,146,658,402,914,82,594,338,850,210,722,466,978,50,562,306,818,178,690,434,946,114,626,370,882,242,754,498,1010,
  10,522,266,778,138,650,394,906,74,586,330,842,202,714,458,970,42,554,298,810,170,682,426,938,106,618,362,874,234,746,490,1002,
  26,538,282,794,154,666,410,922,90,602,346,858,218,730,474,986,58,570,314,826,186,698,442,954,122,634,378,890,250,762,506,1018,
  6,518,262,774,134,646,390,902,70,582,326,838,198,710,454,966,38,550,294,806,166,678,422,934,102,614,358,870,230,742,486,998,
  22,534,278,790,150,662,406,918,86,598,342,854,214,726,470,982,54,566,310,822,182,694,438,950,118,630,374,886,246,758,502,1014,
  14,526,270,782,142,654,398,910,78,590,334,846,206,718,462,974,46,558,302,814,174,686,430,942,110,622,366,878,238,750,494,1006,
  30,542,286,798,158,670,414,926,94,606,350,862,222,734,478,990,62,574,318,830,190,702,446,958,126,638,382,894,254,766,510,1022,
  1,513,257,769,129,641,385,897,65,577,321,833,193,705,449,961,33,545,289,801,161,673,417,929,97,609,353,865,225,737,481,993,
  17,529,273,785,145,657,401,913,81,593,337,849,209,721,465,977,49,561,305,817,177,689,433,945,113,625,369,881,241,753,497,1009,
  9,521,265,777,137,649,393,905,73,585,329,841,201,713,457,969,41,553,297,809,169,681,425,937,105,617,361,873,233,745,489,1001,
  25,537,281,793,153,665,409,921,89,601,345,857,217,729,473,985,57,569,313,825,185,697,441,953,121,633,377,889,249,761,505,1017,
  5,517,261,773,133,645,389,901,69,581,325,837,197,709,453,965,37,549,293,805,165,677,421,933,101,613,357,869,229,741,485,997,
  21,533,277,789,149,661,405,917,85,597,341,853,213,725,469,981,53,565,309,821,181,693,437,949,117,629,373,885,245,757,501,1013,
  13,525,269,781,141,653,397,909,77,589,333,845,205,717,461,973,45,557,301,813,173,685,429,941,109,621,365,877,237,749,493,1005,
  29,541,285,797,157,669,413,925,93,605,349,861,221,733,477,989,61,573,317,829,189,701,445,957,125,637,381,893,253,765,509,1021,
  3,515,259,771,131,643,387,899,67,579,323,835,195,707,451,963,35,547,291,803,163,675,419,931,99,611,355,867,227,739,483,995,
  19,531,275,787,147,659,403,915,83,595,339,851,211,723,467,979,51,563,307,819,179,691,435,947,115,627,371,883,243,755,499,1011,
  11,523,267,779,139,651,395,907,75,587,331,843,203,715,459,971,43,555,299,811,171,683,427,939,107,619,363,875,235,747,491,1003,
  27,539,283,795,155,667,411,923,91,603,347,859,219,731,475,987,59,571,315,827,187,699,443,955,123,635,379,891,251,763,507,1019,
  7,519,263,775,135,647,391,903,71,583,327,839,199,711,455,967,39,551,295,807,167,679,423,935,103,615,359,871,231,743,487,999,
  23,535,279,791,151,663,407,919,87,599,343,855,215,727,471,983,55,567,311,823,183,695,439,951,119,631,375,887,247,759,503,1015,
  15,527,271,783,143,655,399,911,79,591,335,847,207,719,463,975,47,559,303,815,175,687,431,943,111,623,367,879,239,751,495,1007,
  31,543,287,799,159,671,415,927,95,607,351,863,223,735,479,991,63,575,319,831,191,703,447,959,127,639,383,895,255,767,511,1023
};

    unsigned int i,r;
    uint16_t tmp;

    for(i = 0; i < PARAM_N; i++)
    {
        r = bitrev_table[i];
        if (i < r)
        {
          tmp = poly[i];
          poly[i] = poly[r];
          poly[r] = tmp;
        }
    }
}

inline void poly_invntt(poly *r)
{
static const uint16_t omegas_inv_montgomery[PARAM_N/2]	= {4075,5315,4324,4916,10120,11767,7210,9027,10316,6715,1278,9945,3514,11248,11271,5925,147,8500,7840,6833,5537,4749,4467,7500,11099,9606,6171,8471,8429,5445,11239,7753,9090,12233,5529,5206,10587,1987,11635,3565,5415,8646,6153,6427,7341,6152,10561,400,8410,1922,2033,8291,1359,6854,11035,973,8579,6093,6950,5446,11821,8301,11907,316,52,3174,10966,9523,6055,8953,11612,6415,2505,5906,10710,11858,8332,9450,10162,151,3482,787,5468,1010,4169,9162,5241,9369,7509,8844,7232,4698,192,1321,10240,4912,885,6281,10333,7280,8757,11286,58,12048,12147,11184,8812,6608,2844,3438,4212,11314,8687,6068,421,8209,3600,3263,7665,6077,7507,5886,3029,6695,4213,504,11684,2302,1962,1594,6328,7183,168,2692,8960,4298,5184,11089,6122,9734,10929,3956,5297,6170,3762,9370,4016,4077,6523,652,11994,6099,1146,11341,11964,10885,6299,1159,8240,8561,11177,2078,10331,4322,11367,441,4079,11231,3150,1319,8243,709,8049,8719,11454,6224,3054,6803,3123,10542,4433,6370,7032,3834,8633,12225,9830,683,1566,5782,9786,9341,12115,723,3009,1693,5735,2655,2738,6421,11942,2925,1975,8532,3315,11863,4754,1858,1583,6347,2500,10800,6374,1483,12240,1263,1815,5383,10777,350,6920,10232,4493,9087,8855,8760,9381,218,9928,10446,9259,4115,6147,9842,8326,576,10335,10238,10484,9407,6381,11836,8517,418,6860,7515,1293,7552,2767,156,8298,8320,10008,5876,5333,10258,10115,4372,2847,7875,8232,9018,8925,1689,8236,2645,5042,9984,7094,9509,1484,7394,3,4437,160,3149,113,7370,10123,3915,6998,2704,8653,4938,1426,7635,10512,1663,6957,3510,2370,2865,3978,9320,3247,9603,6882,3186,10659,10163,1153,9405,8241,10040,2178,1544,5559,420,8304,4905,476,3531,5191,9153,2399,8889,3000,671,243,3016,3763,10849,12262,9223,10657,7205,11272,7404,7575,8146,10752,242,2678,3704,11744,5019,3833,3778,11899,773,5101,11222,9888,442,2912,5698,11935,4861,7277,9808,11244,2859,3780,11414,4976,10682,7201,8005,11287,5011,6267,2987,2437,3646,2566,10102,9867,6250,5444,2381,11796,8193,4337,11854,1912,1378,404,7644,1065,2143,11121,5277,3248,11082,2548,8058,8907,11934,1759,8582,3694,7110,12144,6747,8652,3459,2731,8357,6378,7399,10861,1696,9863,334,7657,6534,11029,4388,11560,3241,10276,9000,9408,3284,10200,7197,6498,544,2468,339,11267,9,2842,480,5331,7300,1673,4278,4177,8705,9764,1381,7837,2396,8340,8993,4354,130,6915,2837,11462,5767,953,8541,9813,118,7222,2197,3006,9545,563,9314,2625,11340,4821,2639,7266,5828,6561,7698,3328,6512,1351,7311,6553,8155,1305,722,5146,4043,12288,10810,2545,3621,8747,8785,1646,1212,5860,3195,7203,10963,3201,3014,955,11499,9970,11119,3135,3712,7443,9542,7484,8736,9995,11227,1635,9521,1177,8034,140,10436,11563,7678,4320,11289,9198,12208,2963,7393,2366,9238};

static const uint16_t psis_inv_montgomery[PARAM_N] = {256,10570,1510,7238,1034,7170,6291,7921,11665,3422,4000,2327,2088,5565,795,10647,1521,5484,2539,7385,1055,7173,8047,11683,1669,1994,3796,5809,4341,9398,11876,12230,10525,12037,12253,3506,4012,9351,4847,2448,7372,9831,3160,2207,5582,2553,7387,6322,9681,1383,10731,1533,219,5298,4268,7632,6357,9686,8406,4712,9451,10128,4958,5975,11387,8649,11769,6948,11526,12180,1740,10782,6807,2728,7412,4570,4164,4106,11120,12122,8754,11784,3439,5758,11356,6889,9762,11928,1704,1999,10819,12079,12259,7018,11536,1648,1991,2040,2047,2048,10826,12080,8748,8272,8204,1172,1923,7297,2798,7422,6327,4415,7653,6360,11442,12168,7005,8023,9924,8440,8228,2931,7441,1063,3663,5790,9605,10150,1450,8985,11817,10466,10273,12001,3470,7518,1074,1909,7295,9820,4914,702,5367,7789,8135,9940,1420,3714,11064,12114,12264,1752,5517,9566,11900,1700,3754,5803,829,1874,7290,2797,10933,5073,7747,8129,6428,6185,11417,1631,233,5300,9535,10140,11982,8734,8270,2937,10953,8587,8249,2934,9197,4825,5956,4362,9401,1343,3703,529,10609,12049,6988,6265,895,3639,4031,4087,4095,585,10617,8539,4731,4187,9376,3095,9220,10095,10220,1460,10742,12068,1724,5513,11321,6884,2739,5658,6075,4379,11159,10372,8504,4726,9453,3106,7466,11600,10435,8513,9994,8450,9985,3182,10988,8592,2983,9204,4826,2445,5616,6069,867,3635,5786,11360,5134,2489,10889,12089,1727,7269,2794,9177,1311,5454,9557,6632,2703,9164,10087,1441,3717,531,3587,2268,324,5313,759,1864,5533,2546,7386,9833,8427,4715,11207,1601,7251,4547,11183,12131,1733,10781,10318,1474,10744,5046,4232,11138,10369,6748,964,7160,4534,7670,8118,8182,4680,11202,6867,981,8918,1274,182,26,7026,8026,11680,12202,10521,1503,7237,4545,5916,9623,8397,11733,10454,3249,9242,6587,941,1890,270,10572,6777,9746,6659,6218,6155,6146,878,1881,7291,11575,12187,1741,7271,8061,11685,6936,4502,9421,4857,4205,7623,1089,10689,1527,8996,10063,11971,10488,6765,2722,3900,9335,11867,6962,11528,5158,4248,4118,5855,2592,5637,6072,2623,7397,8079,9932,4930,5971,853,3633,519,8852,11798,3441,11025,1575,225,8810,11792,12218,3501,9278,3081,9218,4828,7712,8124,11694,12204,3499,4011,573,3593,5780,7848,9899,10192,1456,208,7052,2763,7417,11593,10434,12024,8740,11782,10461,3250,5731,7841,9898,1414,202,3540,7528,2831,2160,10842,5060,4234,4116,588,84,12,7024,2759,9172,6577,11473,1639,9012,3043,7457,6332,11438,1634,1989,9062,11828,8712,11778,12216,10523,6770,9745,10170,4964,9487,6622,946,8913,6540,6201,4397,9406,8366,9973,8447,8229,11709,8695,10020,3187,5722,2573,10901,6824,4486,4152,9371,8361,2950,2177,311,1800,9035,8313,11721,3430,490,70,10,1757,251,3547,7529,11609,3414,7510,4584,4166,9373,1339,5458,7802,11648,1664,7260,9815,10180,6721,9738,10169,8475,8233,9954,1422,8981,1283,5450,11312,1616,3742,11068,10359,4991,713,3613,9294,8350,4704,672,96,7036,9783,11931,3460,5761,823,10651,12055,10500,1500,5481,783,3623,11051,8601,8251,8201,11705,10450,5004,4226,7626,2845,2162,3820,7568,9859,3164,452,10598,1514,5483,6050,6131,4387,7649,8115,6426,918,8909,8295,1185,5436,11310,8638,1234,5443,11311,5127,2488,2111,10835,5059,7745,2862,3920,560,80,1767,2008,3798,11076,6849,2734,10924,12094,8750,1250,10712,6797,971,7161,1023,8924,4786,7706,4612,4170,7618,6355,4419,5898,11376,10403,10264,6733,4473,639,5358,2521,9138,3061,5704,4326,618,5355,765,5376,768,7132,4530,9425,3102,9221,6584,11474,10417,10266,12000,6981,6264,4406,2385,7363,4563,4163,7617,9866,3165,9230,11852,10471,5007,5982,11388,5138,734,3616,11050,12112,6997,11533,12181,10518,12036,3475,2252,7344,9827,4915,9480,6621,4457,7659,9872,6677,4465,4149,7615,4599,657,3605,515,10607,6782,4480,640,1847,3775,5806,2585,5636,9583,1369,10729,8555,10000,11962,5220,7768,8132,8184,9947,1421,203,29,8782,11788,1684,10774,10317,4985,9490,8378,4708,11206,5112,5997,7879,11659,12199,8765,10030,4944,5973,6120,6141,6144,7900,11662,1666,238,34,3516,5769,9602,8394,9977,6692,956,10670,6791,9748,11926,8726,11780,5194,742,106,8793,10034,3189,10989,5081,4237,5872,4350,2377,10873,6820,6241,11425,10410,10265,3222,5727,9596,4882,2453,2106,3812,11078,12116,5242,4260,11142,8614,11764,12214,5256,4262,4120,11122,5100,11262,5120,2487,5622,9581,8391,8221,2930,10952,12098,6995,6266,9673,4893,699,3611,4027,5842,11368,1624,232,8811,8281,1183,169,8802,3013,2186,5579,797,3625,4029,11109,1587,7249,11569,8675,6506,2685,10917,12093,12261,12285,1755,7273,1039,1904,272,3550,9285,3082,5707,6082,4380,7648,11626,5172,4250,9385,8363,8217,4685,5936,848,8899,6538,934,1889,3781,9318,10109,10222,6727,961,5404,772,5377,9546,8386,1198,8949,3034,2189,7335,4559,5918,2601,10905,5069,9502,3113,7467,8089,11689,5181,9518,8382,2953,3933,4073,4093,7607,8109,2914,5683,4323,11151,1593,10761,6804,972,3650,2277,5592,4310,7638,9869,4921,703,1856,9043,4803,9464,1352,8971,11815,5199,7765,6376,4422,7654,2849,407,8836,6529,7955,2892,9191,1313,10721,12065,12257,1751,9028,8312,2943,2176,3822,546,78,8789,11789,10462,12028,6985,4509,9422,1346,5459,4291,613,10621,6784,9747,3148,7472,2823,5670,810,7138,8042,4660,7688,6365,6176,6149,2634,5643,9584,10147,11983,5223,9524,11894,10477,8519,1217,3685,2282,326,10580,3267,7489,4581,2410,5611,11335,6886,8006,8166,11700,3427,11023,8597,10006,3185,455,65,5276,7776,4622,5927,7869,9902,11948,5218,2501,5624,2559,10899,1557,1978,10816,10323,8497,4725,675,1852,10798,12076,10503,3256,9243,3076,2195,10847,12083,10504,12034,10497};

  bitrev_vector(r->coeffs);
  ntt((uint16_t *)r->coeffs, omegas_inv_montgomery);
  mul_coefficients(r->coeffs, psis_inv_montgomery);
}


inline void encode_a(unsigned char *r, const poly *pk, const unsigned char *seed)
{
  int i;
  poly_tobytes(r, pk);
  for(i=0;i<NEWHOPE_SEED_BYTES;i++)
    r[NEWHOPE_POLY_BYTES+i] = seed[i];
}

inline void decode_a(poly *pk, unsigned char *seed, const unsigned char *r)
{
  int i;
  poly_frombytes(pk, r);
  for(i=0;i<NEWHOPE_SEED_BYTES;i++)
    seed[i] = r[NEWHOPE_POLY_BYTES+i];
}

inline void encode_b(unsigned char *r, const poly *b, const poly *c)
{
  int i;
  poly_tobytes(r,b);
  for(i=0;i<PARAM_N/4;i++)
    r[NEWHOPE_POLY_BYTES+i] = c->coeffs[4*i] | (c->coeffs[4*i+1] << 2) | (c->coeffs[4*i+2] << 4) | (c->coeffs[4*i+3] << 6);
}

inline void decode_b(poly *b, poly *c, const unsigned char *r)
{
  int i;
  poly_frombytes(b, r);
  for(i=0;i<PARAM_N/4;i++)
  {
    c->coeffs[4*i+0] =  r[NEWHOPE_POLY_BYTES+i]       & 0x03;
    c->coeffs[4*i+1] = (r[NEWHOPE_POLY_BYTES+i] >> 2) & 0x03;
    c->coeffs[4*i+2] = (r[NEWHOPE_POLY_BYTES+i] >> 4) & 0x03;
    c->coeffs[4*i+3] = (r[NEWHOPE_POLY_BYTES+i] >> 6);
  }
}

inline int32_t ct_abs(int32_t v)
{
  int32_t mask = v >> 31;
  return (v ^ mask) - mask;
}


inline int32_t f(int32_t *v0, int32_t *v1, int32_t x)
{
  int32_t xit, t, r, b;

  // Next 6 lines compute t = x/PARAM_Q;
  b = x*2730;
  t = b >> 25;
  b = x - t*12289;
  b = 12288 - b;
  b >>= 31;
  t -= b;

  r = t & 1;
  xit = (t>>1);
  *v0 = xit+r; // v0 = round(x/(2*PARAM_Q))

  t -= 1;
  r = t & 1;
  *v1 = (t>>1)+r;

  return ct_abs(x-((*v0)*2*PARAM_Q));
}

inline int32_t g(int32_t x)
{
  int32_t t,c,b;

  // Next 6 lines compute t = x/(4*PARAM_Q);
  b = x*2730;
  t = b >> 27;
  b = x - t*49156;
  b = 49155 - b;
  b >>= 31;
  t -= b;

  c = t & 1;
  t = (t >> 1) + c; // t = round(x/(8*PARAM_Q))

  t *= 8*PARAM_Q;

  return ct_abs(t - x);
}


inline int16_t LDDecode(int32_t xi0, int32_t xi1, int32_t xi2, int32_t xi3)
{
  int32_t t;

  t  = g(xi0);
  t += g(xi1);
  t += g(xi2);
  t += g(xi3);

  t -= 8*PARAM_Q;
  t >>= 31;
  return t&1;
}

inline void helprec(poly *c, const poly *v, RandomNumberGenerator& rng)
{
  int32_t v0[4], v1[4];
  unsigned char rand[32];
  int i;

  rng.randomize(rand, 32);

  for(i=0; i<256; i++)
  {
    unsigned char rbit = (rand[i>>3] >> (i&7)) & 1;
    int32_t k;

    k  = f(v0+0, v1+0, 8*v->coeffs[  0+i] + 4*rbit);
    k += f(v0+1, v1+1, 8*v->coeffs[256+i] + 4*rbit);
    k += f(v0+2, v1+2, 8*v->coeffs[512+i] + 4*rbit);
    k += f(v0+3, v1+3, 8*v->coeffs[768+i] + 4*rbit);

    k = (2*PARAM_Q-1-k) >> 31;

    int32_t v_tmp[4];
    v_tmp[0] = ((~k) & v0[0]) ^ (k & v1[0]);
    v_tmp[1] = ((~k) & v0[1]) ^ (k & v1[1]);
    v_tmp[2] = ((~k) & v0[2]) ^ (k & v1[2]);
    v_tmp[3] = ((~k) & v0[3]) ^ (k & v1[3]);

    c->coeffs[  0+i] = (v_tmp[0] -   v_tmp[3]) & 3;
    c->coeffs[256+i] = (v_tmp[1] -   v_tmp[3]) & 3;
    c->coeffs[512+i] = (v_tmp[2] -   v_tmp[3]) & 3;
    c->coeffs[768+i] = (   -k    + 2*v_tmp[3]) & 3;
  }
}

inline void rec(unsigned char *key, const poly *v, const poly *c)
{
  int i;
  int32_t tmp[4];

  for(i=0;i<32;i++)
    key[i] = 0;

  for(i=0; i<256; i++)
  {
    tmp[0] = 16*PARAM_Q + 8*(int32_t)v->coeffs[  0+i] - PARAM_Q * (2*c->coeffs[  0+i]+c->coeffs[768+i]);
    tmp[1] = 16*PARAM_Q + 8*(int32_t)v->coeffs[256+i] - PARAM_Q * (2*c->coeffs[256+i]+c->coeffs[768+i]);
    tmp[2] = 16*PARAM_Q + 8*(int32_t)v->coeffs[512+i] - PARAM_Q * (2*c->coeffs[512+i]+c->coeffs[768+i]);
    tmp[3] = 16*PARAM_Q + 8*(int32_t)v->coeffs[768+i] - PARAM_Q * (              c->coeffs[768+i]);

    key[i>>3] |= LDDecode(tmp[0], tmp[1], tmp[2], tmp[3]) << (i & 7);
  }
}

/* Based on the public domain implementation in
 * crypto_hash/keccakc512/simple/ from http://bench.cr.yp.to/supercop.html
 * by Ronny Van Keer
 * and the public domain "TweetFips202" implementation
 * from https://twitter.com/tweetfips202
 * by Gilles Van Assche, Daniel J. Bernstein, and Peter Schwabe */

void keccak_absorb(uint64_t *s,
                   unsigned int r,
                   const unsigned char *m, unsigned long long int mlen,
                   unsigned char p)
{
  unsigned long long i;
  unsigned char t[200];

  for (i = 0; i < 25; ++i)
    s[i] = 0;

  while (mlen >= r)
  {
    for (i = 0; i < r / 8; ++i)
       s[i] ^= load_le<u64bit>(m, i);

    Keccak_1600::permute(s);
    mlen -= r;
    m += r;
  }

  for (i = 0; i < r; ++i)
    t[i] = 0;
  for (i = 0; i < mlen; ++i)
    t[i] = m[i];
  t[i] = p;
  t[r - 1] |= 128;
  for (i = 0; i < r / 8; ++i)
     s[i] ^= load_le<u64bit>(t, i);
}

inline void keccak_squeezeblocks(unsigned char *h, unsigned long long int nblocks,
                                 uint64_t *s, unsigned int r)
{
  unsigned int i;
  while(nblocks > 0)
     {
     Keccak_1600::permute(s);

     copy_out_le(h, r, s);

     h += r;
     nblocks--;
     }
}

inline void shake128_absorb(uint64_t *s, const unsigned char *input, unsigned int inputByteLen)
{
  keccak_absorb(s, SHAKE128_RATE, input, inputByteLen, 0x1F);
}

inline void shake128_squeezeblocks(unsigned char *output, unsigned long long nblocks, uint64_t *s)
{
  keccak_squeezeblocks(output, nblocks, s, SHAKE128_RATE);
}

void gen_a(poly *a, const unsigned char *seed)
{
  unsigned int pos=0, ctr=0;
  uint16_t val;
  uint64_t state[25];
  unsigned int nblocks=16;
  uint8_t buf[SHAKE128_RATE*nblocks];

  shake128_absorb(state, seed, NEWHOPE_SEED_BYTES);

  shake128_squeezeblocks((unsigned char *) buf, nblocks, state);

  while(ctr < PARAM_N)
  {
    val = (buf[pos] | ((uint16_t) buf[pos+1] << 8)) & 0x3fff; // Specialized for q = 12889
    if(val < PARAM_Q)
      a->coeffs[ctr++] = val;
    pos += 2;
    if(pos > SHAKE128_RATE*nblocks-2)
    {
      nblocks=1;
      shake128_squeezeblocks((unsigned char *) buf,nblocks,state);
      pos = 0;
    }
  }
}

}

// API FUNCTIONS

void newhope_hash(unsigned char *output, const unsigned char *input, unsigned int inputByteLen)
{
const size_t SHA3_256_RATE = 136;

  uint64_t s[25];
  unsigned char t[SHA3_256_RATE];
  int i;

  keccak_absorb(s, SHA3_256_RATE, input, inputByteLen, 0x06);
  keccak_squeezeblocks(t, 1, s, SHA3_256_RATE);
  for(i=0;i<32;i++)
    output[i] = t[i];
}

void newhope_keygen(unsigned char *send, poly *sk, RandomNumberGenerator& rng)
{
  poly a, e, r, pk;
  unsigned char seed[NEWHOPE_SEED_BYTES];

  rng.randomize(seed, NEWHOPE_SEED_BYTES);

  gen_a(&a, seed);

  poly_getnoise(rng, sk);
  poly_ntt(sk);

  poly_getnoise(rng, &e);
  poly_ntt(&e);

  poly_pointwise(&r,sk,&a);
  poly_add(&pk,&e,&r);

  encode_a(send, &pk, seed);
}

void newhope_sharedb(unsigned char *sharedkey, unsigned char *send, const unsigned char *received,
                     RandomNumberGenerator& rng)
{
  poly sp, ep, v, a, pka, c, epp, bp;
  unsigned char seed[NEWHOPE_SEED_BYTES];

  decode_a(&pka, seed, received);
  gen_a(&a, seed);

  poly_getnoise(rng, &sp);
  poly_ntt(&sp);
  poly_getnoise(rng, &ep);
  poly_ntt(&ep);

  poly_pointwise(&bp, &a, &sp);
  poly_add(&bp, &bp, &ep);

  poly_pointwise(&v, &pka, &sp);
  poly_invntt(&v);

  poly_getnoise(rng, &epp);
  poly_add(&v, &v, &epp);

  helprec(&c, &v, rng);

  encode_b(send, &bp, &c);

  rec(sharedkey, &v, &c);

  newhope_hash(sharedkey, sharedkey, 32);
}


void newhope_shareda(unsigned char *sharedkey, const poly *sk, const unsigned char *received)
{
  poly v,bp, c;

  decode_b(&bp, &c, received);

  poly_pointwise(&v,sk,&bp);
  poly_invntt(&v);

  rec(sharedkey, &v, &c);

  newhope_hash(sharedkey, sharedkey, 32);
}

}

#undef PARAM_N
#undef PARAM_Q
