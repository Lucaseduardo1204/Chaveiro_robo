// =====================================================================
//  Case "cabeça de robô"
//  ESP32-C3 SuperMini + OLED 0.96" + TP4056 USB-C + botão 6x6 + chave slide
//
//  Duas versões de bateria (variável "bateria"):
//    "fone" -> LiPo 602030 300mAh (do fone Multilaser): chaveiro compacto
//    "ps4"  -> bateria do DualShock 4 (LIP1522): robô maior, dura semanas
//
//  Como usar:
//   - Escolhe a bateria, mede as tuas peças e ajusta a seção COMPONENTES.
//   - Troca "parte" e exporta (F6 -> F7 no OpenSCAD):
//       "frente"   -> corpo (imprime com a tela virada pra mesa)
//       "tampa"    -> tampa de trás (imprime com a aba pra cima)
//       "montagem" -> visualização explodida, com as peças fantasmas
//
//  Orientação: x = largura, y = altura (argola em cima),
//              z = profundidade (z=0 é a face da tela)
// =====================================================================

bateria = "fone";     // "fone" ou "ps4"
parte   = "montagem"; // "frente", "tampa" ou "montagem"

$fn = 48;
ps4 = (bateria == "ps4");

// ---------- Paredes ----------
parede = 1.6;
raio   = 3;      // arredondado dos cantos
folga  = 0.2;    // folga da tampa. Ficou dura? sobe pra 0.25. Frouxa? desce pra 0.15

// ---------- COMPONENTES (mede os teus e ajusta!) ----------
// OLED 0.96" I2C
oled_w   = 27.5;
oled_h   = 28.0;
oled_esp = 4.5;          // vidro + placa + componentes de trás
janela_w = 25;           // área visível + margem
janela_h = 14;
janela_offset_y = 1.0;   // + sobe a janela, - desce (confere no teu display)
chanfro  = 1.2;          // bisel da janela

// Bateria (já com folguinha)
//   602030:     30 x 20 x 6 mm
//   DualShock 4: 52~54 x 34 x 7.3~8.1 mm (muda entre a 1ª e a 2ª versão do controle)
bat_w   = ps4 ? 55   : 30;
bat_h   = ps4 ? 35   : 20;
bat_esp = ps4 ? 8.6  : 6.5;
canal_fio = 3;           // vão de cada lado da bateria pros fios do OLED passarem

// ESP32-C3 SuperMini (22.5 x 18 mm)
c3_w = 18;    c3_h = 22.5;
// TP4056 USB-C com proteção (29 x 17.3 x 4.3 mm) + folguinha
tp_w = 17.8;  tp_h = 29.5;
// Camada das placas: C3 e TP4056 lado a lado, USB-C virado pra baixo
placas_esp = 7;

// Furos do USB-C (generosos de propósito)
usb_w   = 10;
usb_alt = 4.6;
usb_z_na_camada = 2.7;   // centro do conector, medido a partir da face da camada das placas

// Botão 6x6 e chave slide (os dois saem pela parede de cima)
botao_d    = 4.2;
chave_comp = 6.5;  chave_larg = 2.4;

// Argola do chaveiro
argola_r    = 4.5;
argola_furo = 2.2;
argola_esp  = 5;

// Aba da tampa (entra no corpo por pressão)
aba_alt = 2.0;
aba_esp = 1.2;

// ---------- Derivados (não precisa mexer) ----------
placas_w = c3_w + tp_w + 0.5;   // C3 + TP4056 lado a lado

// fone: botão e chave ficam ACIMA da bateria (sobra altura).
// ps4:  a bateria ocupa tudo, então eles vão pra camada das placas, ao lado do TP4056.
espaco_botoes = 22;
in_w = max(oled_w + 4,
           bat_w + 2*canal_fio,
           ps4 ? placas_w + espaco_botoes : placas_w);
in_h = max(oled_h + 4,
           tp_h + 4,
           ps4 ? bat_h + 2 : bat_h + 14);   // +14: espaço pro botão e chave acima da bateria
in_d = oled_esp + bat_esp + placas_esp;

W = in_w + 2*parede;
H = in_h + 2*parede;
D_frente = parede + in_d;

z_oled   = parede;
z_bat    = z_oled + oled_esp;
z_placas = z_bat + bat_esp;

c3_x = -in_w/2 + c3_w/2;
tp_x = ps4 ? -in_w/2 + c3_w + 0.5 + tp_w/2   // grudado no C3, libera a direita pros botões
           :  in_w/2 - tp_w/2;

// posição do botão e da chave
livre_x0 = -in_w/2 + placas_w;   // onde começa o espaço livre na camada das placas (ps4)
botao_x  = ps4 ? livre_x0 + 6         : -11;
chave_x  = ps4 ? in_w/2 - 7           :  11;
botoes_z = ps4 ? z_placas + placas_esp/2 : z_bat + bat_esp/2;

// bateria encostada embaixo (fone) ou centralizada (ps4)
bat_y0 = ps4 ? -bat_h/2 : -in_h/2;

echo(str("Bateria: ", bateria, " | tamanho externo: ", W, " x ", H, " x ", D_frente + parede, " mm (sem a argola)"));

// =====================================================================

module caixa_arred(w, h, d, r) {
  hull() for (sx = [-1, 1], sy = [-1, 1])
    translate([sx*(w/2 - r), sy*(h/2 - r), 0]) cylinder(r = r, h = d);
}

module usb_furo() {
  rotate([90, 0, 0]) hull() for (s = [-1, 1])
    translate([s*(usb_w - usb_alt)/2, 0, 0]) cylinder(d = usb_alt, h = parede*3, center = true);
}

// Argola nasce rente à face da tela: imprime sem suporte
module argola() {
  cy = H/2 + argola_r - 1;
  difference() {
    hull() {
      translate([0, cy, 0]) cylinder(r = argola_r, h = argola_esp);
      translate([-argola_r, H/2 - 1, 0]) cube([2*argola_r, 0.1, argola_esp]);
    }
    translate([0, cy, -1]) cylinder(r = argola_furo, h = argola_esp + 2);
  }
}

// Moldurinha que encaixa a placa do OLED no lugar certo
module moldura_oled() {
  m = 0.3; t = 1.0; h = 2.6;
  translate([0, 0, parede - 0.05]) difference() {  // afunda um tico pra fundir com a parede
    translate([-(oled_w/2 + m + t), -(oled_h/2 + m + t), 0])
      cube([oled_w + 2*(m + t), oled_h + 2*(m + t), h]);
    translate([-(oled_w/2 + m), -(oled_h/2 + m), -1])
      cube([oled_w + 2*m, oled_h + 2*m, h + 2]);
  }
}

module frente() {
  difference() {
    union() {
      caixa_arred(W, H, D_frente, raio);
      argola();
    }
    // oco
    translate([0, 0, parede]) caixa_arred(in_w, in_h, in_d + 1, max(raio - parede, 0.5));

    // janela da tela, com bisel por fora
    translate([0, janela_offset_y, 0]) hull() {
      translate([0, 0, -0.01])         cube([janela_w + 2*chanfro, janela_h + 2*chanfro, 0.02], center = true);
      translate([0, 0, parede + 0.01]) cube([janela_w, janela_h, 0.02], center = true);
    }

    // USB-C do C3 (programar) e do TP4056 (carregar)
    for (x = [c3_x, tp_x])
      translate([x, -H/2 + parede/2, z_placas + usb_z_na_camada]) usb_furo();

    // botão
    translate([botao_x, H/2 - parede/2, botoes_z])
      rotate([90, 0, 0]) cylinder(d = botao_d, h = parede + 2, center = true);

    // chave liga/desliga
    translate([chave_x, H/2 - parede/2, botoes_z])
      cube([chave_comp, parede + 2, chave_larg], center = true);

    // rasguinho pra abrir a tampa com a unha (entre os dois USB)
    translate([(c3_x + tp_x)/2, -H/2 + parede/2, D_frente - 0.4])
      cube([3, parede + 1, 1.6], center = true);
  }
  moldura_oled();
}

module tampa() {
  caixa_arred(W, H, parede, raio);
  translate([0, 0, parede - 0.05]) difference() {  // afunda um tico pra fundir com a placa
    caixa_arred(in_w - 2*folga, in_h - 2*folga, aba_alt, max(raio - parede - folga, 0.5));
    translate([0, 0, -1])
      caixa_arred(in_w - 2*folga - 2*aba_esp, in_h - 2*folga - 2*aba_esp, aba_alt + 2, 0.5);
    // ps4: botão e chave estão na camada das placas, então a aba ganha recortes pra não bater neles.
    // Recorte espelhado (+x e -x): não importa pra que lado você vira a tampa, só o lado de cima.
    if (ps4) {
      for (s = [-1, 1]) {
        translate([s*botao_x, in_h/2 - 2, aba_alt/2]) cube([8, 6, aba_alt + 2], center = true);
        translate([s*chave_x, in_h/2 - 2, aba_alt/2]) cube([11, 6, aba_alt + 2], center = true);
      }
    }
  }
}

// Setinha gravada por fora da tampa apontando pra cima (lado da argola)
module seta_topo() {
  translate([0, H/2 - parede - 5, -0.01])
    linear_extrude(0.5) polygon([[-3, 0], [3, 0], [0, 3.5]]);
}

module tampa_final() {
  difference() { tampa(); seta_topo(); }
}

// Peças "fantasma" só pra conferir se tudo cabe (não vão pro STL)
module componentes() {
  // OLED
  color("RoyalBlue") translate([-oled_w/2, -oled_h/2, z_oled]) cube([oled_w, oled_h, oled_esp - 0.3]);
  // bateria
  color("Silver") translate([-bat_w/2 + 0.5, bat_y0 + 0.5, z_bat]) cube([bat_w - 1, bat_h - 1, bat_esp - 0.5]);
  // C3
  color("Black") translate([c3_x - c3_w/2, -in_h/2, z_placas]) cube([c3_w, c3_h, 1]);
  color("Gainsboro") translate([c3_x - 4.5, -in_h/2 - 0.5, z_placas + 1]) cube([9, 7.5, 3.2]);
  // TP4056
  color("SteelBlue") translate([tp_x - tp_w/2, -in_h/2, z_placas]) cube([tp_w, tp_h, 1]);
  color("Gainsboro") translate([tp_x - 4.5, -in_h/2 - 0.5, z_placas + 1]) cube([9, 7.5, 3.2]);
  // botão 6x6
  color("DimGray") translate([botao_x - 3, in_h/2 - 3.5, botoes_z - 3]) cube([6, 3.5, 6]);
  // chave slide
  color("DimGray") translate([chave_x - 4.3, in_h/2 - 3.5, botoes_z - 1.8]) cube([8.6, 3.5, 3.6]);
}

if (parte == "frente") {
  frente();
} else if (parte == "tampa") {
  tampa_final();
} else {
  color("WhiteSmoke") frente();
  componentes();
  color("LightGray") translate([0, 0, D_frente + 15 + parede]) mirror([0, 0, 1]) tampa_final();
}
