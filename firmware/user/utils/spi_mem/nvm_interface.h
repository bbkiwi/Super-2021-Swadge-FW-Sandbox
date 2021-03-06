/*
 * nvm_interface.h
 *
 *  Created on: Oct 13, 2018
 *      Author: adam
 */

#ifndef USER_CUSTOM_COMMANDS_H_
#define USER_CUSTOM_COMMANDS_H_

#define NUM_TT_HIGH_SCORES 3 //Track this many highest scores.
#define NUM_MZ_LEVELS 7 //Track best times for each level

void ICACHE_FLASH_ATTR LoadSettings( void );

void ICACHE_FLASH_ATTR setMuteOverride(bool opt);
void ICACHE_FLASH_ATTR setIsMutedOption(bool mute);
bool ICACHE_FLASH_ATTR getIsMutedOption(void);

#endif /* USER_CUSTOM_COMMANDS_H_ */
