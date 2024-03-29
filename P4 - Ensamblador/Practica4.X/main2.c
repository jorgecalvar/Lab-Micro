#include <xc.h>
#define PIN_PULSADOR 5

int main(void)
{
    int pulsador;
    
    TRISC = 0;  // Configuramos todo como output
    LATC = 0xFFFE;   // El LED es activo a nivel bajo
    TRISB = 1 << PIN_PULSADOR;  // Pin de pulsador como input
    
    while(1){
        // Se lee el estado del pulsador
        pulsador = (PORTB>>PIN_PULSADOR) & 1;
        if(pulsador == 0){
            LATC &= ~1;
        }else{
            LATC |= 1;
        }
    }
    
}


