LED Control (LEDC)
==================
{IDF_TARGET_LEDC_CHAN_NUM:default="8", esp32="16", esp32s2="8", esp32c3="6"}

:link_to_translation:`zh_CN:[中文]`

Introduction
------------

The LED control (LEDC) peripheral is primarily designed to control the intensity of LEDs, although it can also be used to generate PWM signals for other purposes.
It has {IDF_TARGET_LEDC_CHAN_NUM} channels which can generate independent waveforms that can be used, for example, to drive RGB LED devices.

.. only:: esp32

    LEDC channels are divided into two groups of 8 channels each. One group of LEDC channels operates in high speed mode. This mode is implemented in hardware and offers automatic and glitch-free changing of the PWM duty cycle. The other group of channels operate in low speed mode, the PWM duty cycle must be changed by the driver in software. Each group of channels is also able to use different clock sources.

The PWM controller can automatically increase or decrease the duty cycle gradually, allowing for fades without any processor interference.


Functionality Overview
----------------------

.. only:: esp32

    Setting up a channel of the LEDC in either :ref:`high or low speed mode <ledc-api-high_low_speed_mode>` is done in three steps:


.. only:: not esp32

    Setting up a channel of the LEDC is done in three steps. Note that unlike ESP32, {IDF_TARGET_NAME} only supports configuring channels in "low speed" mode.

1. :ref:`ledc-api-configure-timer` by specifying the PWM signal's frequency and duty cycle resolution.
2. :ref:`ledc-api-configure-channel` by associating it with the timer and GPIO to output the PWM signal.
3. :ref:`ledc-api-change-pwm-signal` that drives the output in order to change LED's intensity. This can be done under the full control of software or with hardware fading functions.

As an optional step, it is also possible to set up an interrupt on fade end.

.. figure:: ../../../_static/ledc-api-settings.jpg
    :align: center
    :alt: Key Settings of LED PWM Controller's API
    :figclass: align-center

    Key Settings of LED PWM Controller's API


.. _ledc-api-configure-timer:

Timer Configuration
^^^^^^^^^^^^^^^^^^^

Setting the timer is done by calling the function :cpp:func:`ledc_timer_config` and passing the data structure :cpp:type:`ledc_timer_config_t` that contains the following configuration settings:

.. list::

    :esp32:     - Speed mode :cpp:type:`ledc_mode_t`
    :not esp32: - Speed mode (value must be ``LEDC_LOW_SPEED_MODE``)
    - Timer number :cpp:type:`ledc_timer_t`
    - PWM signal frequency
    - Resolution of PWM duty

The frequency and the duty resolution are interdependent. The higher the PWM frequency, the lower the duty resolution which is available, and vice versa. This relationship might be important if you are planning to use this API for purposes other than changing the intensity of LEDs. For more details, see Section :ref:`ledc-api-supported-range-frequency-duty-resolution`.


.. _ledc-api-configure-channel:

Channel Configuration
^^^^^^^^^^^^^^^^^^^^^

When the timer is set up, configure the desired channel (one out of :cpp:type:`ledc_channel_t`). This is done by calling the function :cpp:func:`ledc_channel_config`.

Similar to the timer configuration, the channel setup function should be passed a structure :cpp:type:`ledc_channel_config_t` that contains the channel's configuration parameters.

At this point, the channel should start operating and generating the PWM signal on the selected GPIO, as configured in :cpp:type:`ledc_channel_config_t`, with the frequency specified in the timer settings and the given duty cycle. The channel operation (signal generation) can be suspended at any time by calling the function :cpp:func:`ledc_stop`.


.. _ledc-api-change-pwm-signal:

Change PWM Signal
^^^^^^^^^^^^^^^^^

Once the channel starts operating and generating the PWM signal with the constant duty cycle and frequency, there are a couple of ways to change this signal. When driving LEDs, primarily the duty cycle is changed to vary the light intensity.

The following two sections describe how to change the duty cycle using software and hardware fading. If required, the signal's frequency can also be changed; it is covered in Section :ref:`ledc-api-change-pwm-frequency`.

.. only:: esp32s2 or esp32c3

    .. note::

        All the timers and channels in the {IDF_TARGET_NAME}'s LED PWM Controller only support low speed mode. Any change of PWM settings must be explicitly triggered by software (see below).


Change PWM Duty Cycle Using Software
""""""""""""""""""""""""""""""""""""

To set the duty cycle, use the dedicated function :cpp:func:`ledc_set_duty`. After that, call :cpp:func:`ledc_update_duty` to activate the changes. To check the currently set value, use the corresponding ``_get_`` function :cpp:func:`ledc_get_duty`.

Another way to set the duty cycle, as well as some other channel parameters, is by calling :cpp:func:`ledc_channel_config` covered in Section :ref:`ledc-api-configure-channel`.

The range of the duty cycle values passed to functions depends on selected ``duty_resolution`` and should be from ``0`` to ``(2 ** duty_resolution)``. For example, if the selected duty resolution is 10, then the duty cycle values can range from 0 to 1024. This provides the resolution of ~ 0.1%.

.. only:: esp32 or esp32s2 or esp32s3 or esp32c3

    .. warning::

        On {IDF_TARGET_NAME}, when channel's binded timer selects its maximum duty resolution, the duty cycle value cannot be set to ``(2 ** duty_resolution)``. Otherwise, the internal duty counter in the hardware will overflow and be messed up.


Change PWM Duty Cycle using Hardware
""""""""""""""""""""""""""""""""""""

The LEDC hardware provides the means to gradually transition from one duty cycle value to another. To use this functionality, enable fading with :cpp:func:`ledc_fade_func_install` and then configure it by calling one of the available fading functions:

* :cpp:func:`ledc_set_fade_with_time`
* :cpp:func:`ledc_set_fade_with_step`
* :cpp:func:`ledc_set_fade`

Start fading with :cpp:func:`ledc_fade_start`. A fade can be operated in blocking or non-blocking mode, please check :cpp:enum:`ledc_fade_mode_t` for the difference between the two available fade modes. Note that with either fade mode, the next fade or fixed-duty update will not take effect until the last fade finishes.

To get a notification about the completion of a fade operation, a fade end callback function can be registered for each channel by calling :cpp:func:`ledc_cb_register` after the fade service being installed. The fade end callback prototype is defined in :cpp:type:`ledc_cb_t`, where you should return a boolean value from the callback function, indicating whether a high priority task is woken up by this callback function. It is worth mentioning, the callback and the function invoked by itself should be placed in IRAM, as the interrupt service routine is in IRAM. :cpp:func:`ledc_cb_register` will print a warning message if it finds the addresses of callback and user context are incorrect.

If not required anymore, fading and an associated interrupt can be disabled with :cpp:func:`ledc_fade_func_uninstall`.


.. _ledc-api-change-pwm-frequency:

Change PWM Frequency
""""""""""""""""""""

The LEDC API provides several ways to change the PWM frequency "on the fly":

    * Set the frequency by calling :cpp:func:`ledc_set_freq`. There is a corresponding function :cpp:func:`ledc_get_freq` to check the current frequency.
    * Change the frequency and the duty resolution by calling :cpp:func:`ledc_bind_channel_timer` to bind some other timer to the channel.
    * Change the channel's timer by calling :cpp:func:`ledc_channel_config`.


More Control Over PWM
"""""""""""""""""""""

There are several lower level timer-specific functions that can be used to change PWM settings:

* :cpp:func:`ledc_timer_set`
* :cpp:func:`ledc_timer_rst`
* :cpp:func:`ledc_timer_pause`
* :cpp:func:`ledc_timer_resume`

The first two functions are called "behind the scenes" by :cpp:func:`ledc_channel_config` to provide a startup of a timer after it is configured.


Use Interrupts
^^^^^^^^^^^^^^

When configuring an LEDC channel, one of the parameters selected within :cpp:type:`ledc_channel_config_t` is :cpp:type:`ledc_intr_type_t` which triggers an interrupt on fade completion.

For registration of a handler to address this interrupt, call :cpp:func:`ledc_isr_register`.


.. only:: esp32

    .. _ledc-api-high_low_speed_mode:

    LEDC High and Low Speed Mode
    ----------------------------

    High speed mode enables a glitch-free changeover of timer settings. This means that if the timer settings are modified, the changes will be applied automatically on the next overflow interrupt of the timer. In contrast, when updating the low-speed timer, the change of settings should be explicitly triggered by software. The LEDC driver handles it in the background, e.g., when :cpp:func:`ledc_timer_config` or :cpp:func:`ledc_timer_set` is called.

    For additional details regarding speed modes, see *{IDF_TARGET_NAME} Technical Reference Manual* > *LED PWM Controller (LEDC)* [`PDF <{IDF_TARGET_TRM_EN_URL}#ledpwm>`__]. Please note that the support for ``SLOW_CLOCK`` mentioned in this manual is not yet supported in the LEDC driver.

    .. _ledc-api-supported-range-frequency-duty-resolution:

.. only:: not esp32

    .. _ledc-api-supported-range-frequency-duty-resolution:

Supported Range of Frequency and Duty Resolutions
-------------------------------------------------

The LED PWM Controller is designed primarily to drive LEDs. It provides a large flexibility of PWM duty cycle settings. For instance, the PWM frequency of 5 kHz can have the maximum duty resolution of 13 bits. This means that the duty can be set anywhere from 0 to 100% with a resolution of ~0.012% (2 ** 13 = 8192 discrete levels of the LED intensity). Note, however, that these parameters depend on the clock signal clocking the LED PWM Controller timer which in turn clocks the channel (see :ref:`timer configuration<ledc-api-configure-timer>` and the *{IDF_TARGET_NAME} Technical Reference Manual* > *LED PWM Controller (LEDC)* [`PDF <{IDF_TARGET_TRM_EN_URL}#ledpwm>`__]).

The LEDC can be used for generating signals at much higher frequencies that are sufficient enough to clock other devices, e.g., a digital camera module. In this case, the maximum available frequency is 40 MHz with duty resolution of 1 bit. This means that the duty cycle is fixed at 50% and cannot be adjusted.

The LEDC API is designed to report an error when trying to set a frequency and a duty resolution that exceed the range of LEDC's hardware. For example, an attempt to set the frequency to 20 MHz and the duty resolution to 3 bits will result in the following error reported on a serial monitor:

.. highlight:: none

::

    E (196) ledc: requested frequency and duty resolution cannot be achieved, try reducing freq_hz or duty_resolution. div_param=128

In such a situation, either the duty resolution or the frequency must be reduced. For example, setting the duty resolution to 2 will resolve this issue and will make it possible to set the duty cycle at 25% steps, i.e., at 25%, 50% or 75%.

The LEDC driver will also capture and report attempts to configure frequency / duty resolution combinations that are below the supported minimum, e.g.:

::

    E (196) ledc: requested frequency and duty resolution cannot be achieved, try increasing freq_hz or duty_resolution. div_param=128000000

The duty resolution is normally set using :cpp:type:`ledc_timer_bit_t`. This enumeration covers the range from 10 to 15 bits. If a smaller duty resolution is required (from 10 down to 1), enter the equivalent numeric values directly.


Application Example
-------------------

The LEDC change duty cycle and fading control example: :example:`peripherals/ledc/ledc_fade`.

The LEDC basic example: :example:`peripherals/ledc/ledc_basic`.

API Reference
-------------

.. include-build-file:: inc/ledc.inc
.. include-build-file:: inc/ledc_types.inc
