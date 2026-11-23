size_option = ARGV.find { |argument| argument.start_with?("--size=") }
match_option = ARGV.find { |argument| argument.start_with?("--match-rate=") }

abort "MISSING --size=N!!!!" if size_option.nil?

begin
  size = Integer(size_option.split("=", 2)[1])
  match_rate = Float(match_option&.split("=", 2)&.[](1) || "1.0")
rescue ArgumentError
  abort "INVALID GENERATOR ARGUMENT!!!!"
end

abort "SIZE MUST BE POSITIVE!!!!" if size <= 0
unless match_rate.between?(0.0, 1.0)
  abort "MATCH RATE MUST BE BETWEEN 0 AND 1!!!!"
end

matching_rows = (size * match_rate).round

puts "3"

print "relation R(a=number,b=number){"
size.times { |i| print "#{i},#{i};" }
puts "};"

print "relation S(b=number,c=number){"
size.times do |i|
  b = i < matching_rows ? i : size + i
  print "#{b},#{i};"
end
puts "};"

puts "R@{R.b=S.b}S;"
puts ":q"
