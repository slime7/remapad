export default {
  target: 'embedded',
  arch: 'esp32s3',
  display: {
    width: 240,
    height: 280,
    format: 'rgb565',
  },
  entry: 'src/index.jsx',
  outDir: 'dist',
};
