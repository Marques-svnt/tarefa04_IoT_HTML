#ifndef DS18B20_H
#define DS18B20_H

// Funções para uso do sensor de temperatura ds18b20
void ds18b20_init(uint pin); 
bool ds18b20_reset(void);
float ds18b20_get_temperature(void);

#endif 