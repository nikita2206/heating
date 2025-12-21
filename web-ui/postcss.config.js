import purgecss from '@fullhuman/postcss-purgecss';
import cssnano from 'cssnano';

export default {
  plugins: [
    purgecss({
      content: ['./index.html', './src/**/*.js'],
      safelist: [
        'active',
        'connected',
        'dragover',
        'uploading',
        'success',
        'error',
        'request',
        'response',
        'status',
        'gateway-boiler',
        'thermostat-gateway',
        'thermostat-boiler',
        'green',
        'yellow',
        'red',
        'purple',
        'ok',
        'bad',
        'invalid'
      ]
    }),
    cssnano({ preset: 'default' })
  ]
};
