int imon_open(pa_mainloop_api *api);
void imon_close(pa_mainloop_api *api);
void imon_write(const void *data, int length);
void imon_clear();

