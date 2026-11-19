/* Calls and USSD through the modem's QMI voice service (drivers/modem/hakios.voice.c). */
#ifndef _SYS_HAKIOS_VOICE_H_
#define _SYS_HAKIOS_VOICE_H_

struct hakios_qrtr_ops;

/* The client, on that processor's QRTR transport: opened once the modem's services are there; 0, or an errno. */
int hakios_voice_open(const struct hakios_qrtr_ops *transport);
/* ... and closed before the link to the modem goes. */
void hakios_voice_close(void);

#endif /* _SYS_HAKIOS_VOICE_H_ */
