int main(void){
  int t0 = millis();
  plot_clear();
  plot_fill_rect(10,10,60,40, rgb(200,40,40));
  plot_circle(160,160,50, rgb(60,200,90));
  plot_fill_circle(260,80,30, rgb(40,80,220));
  plot_line(0,319,319,0, rgb(255,255,255));
  plot_text(12,300,"calcos 2.0", rgb(255,220,80));
  plot_show();
  printf("drew in %d ms, %dx%d\n", millis()-t0, plot_width(), plot_height());
  return 0;
}
