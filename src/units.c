/* Physical units and the constants that carry them.
 *
 * A value's unit is seven small integers: the exponents of metre,
 * kilogram, second, ampere, kelvin, mole and candela. Everything is held
 * in SI base units, so a quantity entered as 3 km is 3000 with the metre
 * exponent set, and arithmetic is then just arithmetic on the numbers with
 * the exponents added or subtracted alongside. All zero means
 * dimensionless, which is what an ordinary number already is: that is why
 * units cost nothing for a calculator used without them.
 *
 * Addition and subtraction require the same exponents, which is the check
 * that makes the whole thing worth having. Multiplying metres by seconds
 * is always meaningful; adding them never is.
 */
#include "calc.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* the unit table                                                      */
/* ------------------------------------------------------------------ */

/* {name, factor to SI, m, kg, s, A, K, mol, cd} */
typedef struct {
    const char *name;
    double      factor;
    signed char d[UNIT_DIMS];
} UnitDef;

static const UnitDef units[] = {
    /* base */
    { "m",    1.0,          { 1, 0, 0, 0, 0, 0, 0 } },
    { "kg",   1.0,          { 0, 1, 0, 0, 0, 0, 0 } },
    { "s",    1.0,          { 0, 0, 1, 0, 0, 0, 0 } },
    { "A",    1.0,          { 0, 0, 0, 1, 0, 0, 0 } },
    { "K",    1.0,          { 0, 0, 0, 0, 1, 0, 0 } },
    { "mol",  1.0,          { 0, 0, 0, 0, 0, 1, 0 } },
    { "cd",   1.0,          { 0, 0, 0, 0, 0, 0, 1 } },

    /* length */
    { "km",   1000.0,       { 1, 0, 0, 0, 0, 0, 0 } },
    { "cm",   0.01,         { 1, 0, 0, 0, 0, 0, 0 } },
    { "mm",   0.001,        { 1, 0, 0, 0, 0, 0, 0 } },
    { "um",   1e-6,         { 1, 0, 0, 0, 0, 0, 0 } },
    { "nm",   1e-9,         { 1, 0, 0, 0, 0, 0, 0 } },
    { "in",   0.0254,       { 1, 0, 0, 0, 0, 0, 0 } },
    { "ft",   0.3048,       { 1, 0, 0, 0, 0, 0, 0 } },
    { "yd",   0.9144,       { 1, 0, 0, 0, 0, 0, 0 } },
    { "mile", 1609.344,     { 1, 0, 0, 0, 0, 0, 0 } },
    { "nmi",  1852.0,       { 1, 0, 0, 0, 0, 0, 0 } },
    { "au",   1.495978707e11, { 1, 0, 0, 0, 0, 0, 0 } },
    { "ly",   9.4607304725808e15, { 1, 0, 0, 0, 0, 0, 0 } },
    { "pc",   3.0856775814913673e16, { 1, 0, 0, 0, 0, 0, 0 } },
    { "angstrom", 1e-10,    { 1, 0, 0, 0, 0, 0, 0 } },

    /* mass */
    { "g",    0.001,        { 0, 1, 0, 0, 0, 0, 0 } },
    { "mg",   1e-6,         { 0, 1, 0, 0, 0, 0, 0 } },
    { "tonne", 1000.0,      { 0, 1, 0, 0, 0, 0, 0 } },
    { "lb",   0.45359237,   { 0, 1, 0, 0, 0, 0, 0 } },
    { "oz",   0.028349523125, { 0, 1, 0, 0, 0, 0, 0 } },

    /* time */
    { "ms",   0.001,        { 0, 0, 1, 0, 0, 0, 0 } },
    { "us",   1e-6,         { 0, 0, 1, 0, 0, 0, 0 } },
    { "ns",   1e-9,         { 0, 0, 1, 0, 0, 0, 0 } },
    { "min",  60.0,         { 0, 0, 1, 0, 0, 0, 0 } },
    { "hour", 3600.0,       { 0, 0, 1, 0, 0, 0, 0 } },
    { "day",  86400.0,      { 0, 0, 1, 0, 0, 0, 0 } },
    { "week", 604800.0,     { 0, 0, 1, 0, 0, 0, 0 } },
    { "year", 31557600.0,   { 0, 0, 1, 0, 0, 0, 0 } },

    /* derived, named */
    { "Hz",   1.0,          { 0, 0, -1, 0, 0, 0, 0 } },
    { "N",    1.0,          { 1, 1, -2, 0, 0, 0, 0 } },
    { "Pa",   1.0,          { -1, 1, -2, 0, 0, 0, 0 } },
    { "J",    1.0,          { 2, 1, -2, 0, 0, 0, 0 } },
    { "W",    1.0,          { 2, 1, -3, 0, 0, 0, 0 } },
    { "C",    1.0,          { 0, 0, 1, 1, 0, 0, 0 } },
    { "V",    1.0,          { 2, 1, -3, -1, 0, 0, 0 } },
    { "F",    1.0,          { -2, -1, 4, 2, 0, 0, 0 } },
    { "ohm",  1.0,          { 2, 1, -3, -2, 0, 0, 0 } },
    { "S",    1.0,          { -2, -1, 3, 2, 0, 0, 0 } },
    { "Wb",   1.0,          { 2, 1, -2, -1, 0, 0, 0 } },
    { "T",    1.0,          { 0, 1, -2, -1, 0, 0, 0 } },
    { "H",    1.0,          { 2, 1, -2, -2, 0, 0, 0 } },
    { "lm",   1.0,          { 0, 0, 0, 0, 0, 0, 1 } },
    { "Bq",   1.0,          { 0, 0, -1, 0, 0, 0, 0 } },
    { "Gy",   1.0,          { 2, 0, -2, 0, 0, 0, 0 } },

    /* derived, scaled */
    { "kN",   1000.0,       { 1, 1, -2, 0, 0, 0, 0 } },
    { "kHz",  1000.0,       { 0, 0, -1, 0, 0, 0, 0 } },
    { "MHz",  1e6,          { 0, 0, -1, 0, 0, 0, 0 } },
    { "GHz",  1e9,          { 0, 0, -1, 0, 0, 0, 0 } },
    { "kJ",   1000.0,       { 2, 1, -2, 0, 0, 0, 0 } },
    { "MJ",   1e6,          { 2, 1, -2, 0, 0, 0, 0 } },
    { "eV",   1.602176634e-19, { 2, 1, -2, 0, 0, 0, 0 } },
    { "keV",  1.602176634e-16, { 2, 1, -2, 0, 0, 0, 0 } },
    { "MeV",  1.602176634e-13, { 2, 1, -2, 0, 0, 0, 0 } },
    { "cal",  4.184,        { 2, 1, -2, 0, 0, 0, 0 } },
    { "kcal", 4184.0,       { 2, 1, -2, 0, 0, 0, 0 } },
    { "kWh",  3.6e6,        { 2, 1, -2, 0, 0, 0, 0 } },
    { "mW",   0.001,        { 2, 1, -3, 0, 0, 0, 0 } },
    { "kW",   1000.0,       { 2, 1, -3, 0, 0, 0, 0 } },
    { "MW",   1e6,          { 2, 1, -3, 0, 0, 0, 0 } },
    { "hp",   745.6998715823, { 2, 1, -3, 0, 0, 0, 0 } },
    { "mV",   0.001,        { 2, 1, -3, -1, 0, 0, 0 } },
    { "kV",   1000.0,       { 2, 1, -3, -1, 0, 0, 0 } },
    { "mA",   0.001,        { 0, 0, 0, 1, 0, 0, 0 } },
    { "uA",   1e-6,         { 0, 0, 0, 1, 0, 0, 0 } },
    { "kohm", 1000.0,       { 2, 1, -3, -2, 0, 0, 0 } },
    { "Mohm", 1e6,          { 2, 1, -3, -2, 0, 0, 0 } },
    { "uF",   1e-6,         { -2, -1, 4, 2, 0, 0, 0 } },
    { "nF",   1e-9,         { -2, -1, 4, 2, 0, 0, 0 } },
    { "pF",   1e-12,        { -2, -1, 4, 2, 0, 0, 0 } },
    { "mH",   0.001,        { 2, 1, -2, -2, 0, 0, 0 } },
    { "uH",   1e-6,         { 2, 1, -2, -2, 0, 0, 0 } },
    { "kPa",  1000.0,       { -1, 1, -2, 0, 0, 0, 0 } },
    { "MPa",  1e6,          { -1, 1, -2, 0, 0, 0, 0 } },
    { "bar",  1e5,          { -1, 1, -2, 0, 0, 0, 0 } },
    { "atm",  101325.0,     { -1, 1, -2, 0, 0, 0, 0 } },
    { "psi",  6894.757293168, { -1, 1, -2, 0, 0, 0, 0 } },
    { "mmHg", 133.322387415, { -1, 1, -2, 0, 0, 0, 0 } },
    { "gauss", 1e-4,        { 0, 1, -2, -1, 0, 0, 0 } },

    /* area and volume */
    { "L",    0.001,        { 3, 0, 0, 0, 0, 0, 0 } },
    { "mL",   1e-6,         { 3, 0, 0, 0, 0, 0, 0 } },
    { "gal",  0.003785411784, { 3, 0, 0, 0, 0, 0, 0 } },
    { "ha",   1e4,          { 2, 0, 0, 0, 0, 0, 0 } },
    { "acre", 4046.8564224, { 2, 0, 0, 0, 0, 0, 0 } },

    /* speed */
    { "kph",  0.2777777777777778, { 1, 0, -1, 0, 0, 0, 0 } },
    { "mph",  0.44704,      { 1, 0, -1, 0, 0, 0, 0 } },
    { "knot", 0.5144444444444445, { 1, 0, -1, 0, 0, 0, 0 } },
};

#define NUNITS ((int)(sizeof units / sizeof units[0]))

/* Names printed for a dimension vector that matches a familiar derived
 * unit: "N" reads better than "kg m / s^2". Only exact SI-factor units
 * belong here. */
static const char *named_for[] = { "N", "J", "W", "Pa", "V", "ohm", "F",
                                   "Wb", "T", "H", "S", "C", "Hz" };

int unit_count(void) { return NUNITS; }
const char *unit_name(int i)
{
    return (i >= 0 && i < NUNITS) ? units[i].name : 0;
}

int unit_lookup(const char *name, double *factor, Unit *u)
{
    int i, k;
    for (i = 0; i < NUNITS; i++) {
        if (strcmp(units[i].name, name) != 0) continue;
        if (factor) *factor = units[i].factor;
        if (u) for (k = 0; k < UNIT_DIMS; k++) u->d[k] = units[i].d[k];
        return 1;
    }
    return 0;
}

int unit_is_none(Unit u)
{
    int i;
    for (i = 0; i < UNIT_DIMS; i++) if (u.d[i]) return 0;
    return 1;
}

int unit_same(Unit a, Unit b)
{
    int i;
    for (i = 0; i < UNIT_DIMS; i++) if (a.d[i] != b.d[i]) return 0;
    return 1;
}

Unit unit_none(void) { Unit u; memset(&u, 0, sizeof u); return u; }

Unit unit_mul(Unit a, Unit b)
{
    int i;
    for (i = 0; i < UNIT_DIMS; i++) a.d[i] = (signed char)(a.d[i] + b.d[i]);
    return a;
}

Unit unit_div(Unit a, Unit b)
{
    int i;
    for (i = 0; i < UNIT_DIMS; i++) a.d[i] = (signed char)(a.d[i] - b.d[i]);
    return a;
}

Unit unit_pow(Unit a, int n)
{
    int i;
    for (i = 0; i < UNIT_DIMS; i++) a.d[i] = (signed char)(a.d[i] * n);
    return a;
}

/* ------------------------------------------------------------------ */
/* printing                                                            */
/* ------------------------------------------------------------------ */

static const char *base_names[UNIT_DIMS] = { "m", "kg", "s", "A", "K", "mol", "cd" };

void unit_str(Unit u, char *out, size_t cap)
{
    static const char *order[] = { 0 };
    char num[64], den[64];
    int i, nn = 0, nd = 0;
    size_t k;

    (void)order;
    out[0] = 0;
    if (unit_is_none(u)) return;

    /* a familiar name beats a product of base units */
    for (k = 0; k < sizeof named_for / sizeof named_for[0]; k++) {
        Unit nu;
        double f;
        if (!unit_lookup(named_for[k], &f, &nu)) continue;
        if (f == 1.0 && unit_same(u, nu)) {
            snprintf(out, cap, "%s", named_for[k]);
            return;
        }
    }

    num[0] = den[0] = 0;
    for (i = 0; i < UNIT_DIMS; i++) {
        char part[24];
        int e = u.d[i];
        if (!e) continue;
        if (e > 0) {
            if (e == 1) snprintf(part, sizeof part, "%s", base_names[i]);
            else snprintf(part, sizeof part, "%s^%d", base_names[i], e);
            if (nn++) strncat(num, " ", sizeof num - strlen(num) - 1);
            strncat(num, part, sizeof num - strlen(num) - 1);
        } else {
            if (e == -1) snprintf(part, sizeof part, "%s", base_names[i]);
            else snprintf(part, sizeof part, "%s^%d", base_names[i], -e);
            if (nd++) strncat(den, " ", sizeof den - strlen(den) - 1);
            strncat(den, part, sizeof den - strlen(den) - 1);
        }
    }
    /* More than one factor below the line needs brackets, or "m/s K" can
     * be read as metres per second, times kelvin. */
    if (!nn) snprintf(out, cap, nd > 1 ? "1/(%s)" : "1/%s", den);
    else if (!nd) snprintf(out, cap, "%s", num);
    else snprintf(out, cap, nd > 1 ? "%s/(%s)" : "%s/%s", num, den);
}

/* ------------------------------------------------------------------ */
/* physical constants                                                  */
/* ------------------------------------------------------------------ */

/* CODATA 2018. The first seven are exact by definition of the SI. */
typedef struct {
    const char *name;
    double      value;
    const char *unit;        /* as the table above spells it */
    signed char d[UNIT_DIMS];
    const char *what;
} ConstDef;

static const ConstDef consts[] = {
 { "c_light", 299792458.0,        "m/s",   { 1, 0, -1, 0, 0, 0, 0 },
   "speed of light in vacuum (exact)" },
 { "h_planck", 6.62607015e-34,    "J s",   { 2, 1, -1, 0, 0, 0, 0 },
   "Planck constant (exact)" },
 { "hbar", 1.054571817646156e-34, "J s",   { 2, 1, -1, 0, 0, 0, 0 },
   "reduced Planck constant" },
 { "q_e", 1.602176634e-19,        "C",     { 0, 0, 1, 1, 0, 0, 0 },
   "elementary charge (exact)" },
 { "k_B", 1.380649e-23,           "J/K",   { 2, 1, -2, 0, -1, 0, 0 },
   "Boltzmann constant (exact)" },
 { "N_A", 6.02214076e23,          "1/mol", { 0, 0, 0, 0, 0, -1, 0 },
   "Avogadro constant (exact)" },
 { "R_gas", 8.31446261815324,     "J/K/mol", { 2, 1, -2, 0, -1, -1, 0 },
   "molar gas constant (exact)" },
 { "G_grav", 6.67430e-11,         "m^3/kg/s^2", { 3, -1, -2, 0, 0, 0, 0 },
   "Newtonian constant of gravitation" },
 { "g0", 9.80665,                 "m/s^2", { 1, 0, -2, 0, 0, 0, 0 },
   "standard acceleration of gravity (exact)" },
 { "m_e", 9.1093837015e-31,       "kg",    { 0, 1, 0, 0, 0, 0, 0 },
   "electron mass" },
 { "m_p", 1.67262192369e-27,      "kg",    { 0, 1, 0, 0, 0, 0, 0 },
   "proton mass" },
 { "m_n", 1.67492749804e-27,      "kg",    { 0, 1, 0, 0, 0, 0, 0 },
   "neutron mass" },
 { "amu", 1.66053906660e-27,      "kg",    { 0, 1, 0, 0, 0, 0, 0 },
   "atomic mass constant" },
 { "eps0", 8.8541878128e-12,      "F/m",   { -3, -1, 4, 2, 0, 0, 0 },
   "vacuum electric permittivity" },
 { "mu0", 1.25663706212e-6,       "H/m",   { 1, 1, -2, -2, 0, 0, 0 },
   "vacuum magnetic permeability" },
 { "sigma_SB", 5.670374419e-8,    "W/m^2/K^4", { 0, 1, -3, 0, -4, 0, 0 },
   "Stefan-Boltzmann constant (exact)" },
 { "R_inf", 10973731.568160,      "1/m",   { -1, 0, 0, 0, 0, 0, 0 },
   "Rydberg constant" },
 { "a0", 5.29177210903e-11,       "m",     { 1, 0, 0, 0, 0, 0, 0 },
   "Bohr radius" },
 { "mu_B", 9.2740100783e-24,      "J/T",   { 2, 0, 0, 1, 0, 0, 0 },
   "Bohr magneton" },
 { "alpha_fs", 7.2973525693e-3,   "",      { 0, 0, 0, 0, 0, 0, 0 },
   "fine-structure constant" },
 { "atm_p", 101325.0,             "Pa",    { -1, 1, -2, 0, 0, 0, 0 },
   "standard atmosphere (exact)" },
 { "T_zero", 273.15,              "K",     { 0, 0, 0, 0, 1, 0, 0 },
   "zero Celsius in kelvin (exact)" },
};

#define NCONSTS ((int)(sizeof consts / sizeof consts[0]))

int const_count(void) { return NCONSTS; }

int const_at(int i, const char **name, double *value, Unit *u,
             const char **what, const char **unit_text)
{
    int k;
    if (i < 0 || i >= NCONSTS) return 0;
    if (name) *name = consts[i].name;
    if (value) *value = consts[i].value;
    if (what) *what = consts[i].what;
    if (unit_text) *unit_text = consts[i].unit;
    if (u) for (k = 0; k < UNIT_DIMS; k++) u->d[k] = consts[i].d[k];
    return 1;
}

int const_lookup(const char *name, double *value, Unit *u)
{
    int i;
    for (i = 0; i < NCONSTS; i++)
        if (strcmp(consts[i].name, name) == 0)
            return const_at(i, 0, value, u, 0, 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* parsing a unit expression, as given to conv() and to()              */
/* ------------------------------------------------------------------ */

/* Accepts things like "km", "m/s", "kg m/s^2", "N m". Returns 0 if any
 * name is unknown. */
int unit_parse(const char *s, double *factor, Unit *u)
{
    double f = 1.0;
    Unit acc = unit_none();
    int den = 0;

    while (*s) {
        char name[24];
        int n = 0, exp = 1, k;
        double uf;
        Unit uu;

        while (*s == ' ' || *s == '*') s++;
        if (*s == '/') { den = 1; s++; continue; }
        if (!*s) break;
        while (*s && *s != ' ' && *s != '/' && *s != '^' && *s != '*'
               && n < (int)sizeof name - 1)
            name[n++] = *s++;
        name[n] = 0;
        if (!n) return 0;
        if (*s == '^') {
            int sign = 1;
            s++;
            if (*s == '-') { sign = -1; s++; }
            exp = 0;
            while (*s >= '0' && *s <= '9') exp = exp * 10 + (*s++ - '0');
            exp *= sign;
        }
        if (!unit_lookup(name, &uf, &uu)) return 0;
        for (k = 0; k < (exp < 0 ? -exp : exp); k++) {
            if ((exp < 0) != (den != 0)) { f /= uf; acc = unit_div(acc, uu); }
            else { f *= uf; acc = unit_mul(acc, uu); }
        }
    }
    if (factor) *factor = f;
    if (u) *u = acc;
    return 1;
}
