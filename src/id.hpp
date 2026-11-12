/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef _ID_H
#define _ID_H
#pragma once
/* Public include ------------------------------------------------------------*/
#include <functional>

/* Public namespace ----------------------------------------------------------*/
using namespace std;

/* Public define -------------------------------------------------------------*/
/* Public typedef ------------------------------------------------------------*/
/* Public template -----------------------------------------------------------*/
/* Public function prototypes ------------------------------------------------*/
/* Public class --------------------------------------------------------------*/
struct _ID {
  function<const char *()> filename;
};

static _ID id;

#endif /* _ID_H */
