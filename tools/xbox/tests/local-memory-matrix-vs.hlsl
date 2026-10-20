// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
float4 main(uint id : SV_VertexID) : SV_Position {
    return float4(id==2?3:-1,id==1?3:-1,0.5,1);
}
