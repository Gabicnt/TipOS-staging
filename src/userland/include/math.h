#ifndef MATH_H
#define MATH_H
double fabs(double x);
double floor(double x);
double ceil(double x);
double sqrt(double x);
double sin(double x);
double cos(double x);
double atan(double x);
double atan2(double y, double x);
double pow(double x, double y);
double fmod(double x, double y);
float fabsf(float x);
#define HUGE_VAL (__builtin_huge_val())
#endif