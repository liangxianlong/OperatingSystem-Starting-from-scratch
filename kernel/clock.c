/**
 *	clock handler
 *	
 *  <Ring 0> This routine handles the clock interrupt generated by 8253/8254
 *  programmable interval timer
 *  
 *	@param irq The IRQ nr, unused here.
 */
PUBLIC void clock_handler(int irq)
{
	if (++ticks >= MAX_TICKS)
		ticks = 0;
	if (p_proc_ready->ticks)
		p_proc_ready->ticks-;
	if (key_pressed)
		inform_int(TASK_TTY);
	if (k_reenter != 0)
		return;
	if (p_proc_ready->ticks > 0)
		return;

	schedule();
}